#include "nebulabt/core/engine.hpp"

#include "nebulabt/core/scheduler.hpp"
#include "nebulabt/nodes/for_each.hpp"

#include <behaviortree_cpp/behavior_tree.h>
#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/decorator_node.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

namespace nebulabt {

// ============================================================================
// TaskState / TaskHandle 前置实现
// ============================================================================

const char* toString(TaskState s) {
  switch (s) {
    case TaskState::Queued:    return "QUEUED";
    case TaskState::Running:   return "RUNNING";
    case TaskState::Succeeded: return "SUCCEEDED";
    case TaskState::Failed:    return "FAILED";
    case TaskState::Canceled:  return "CANCELED";
  }
  return "UNKNOWN";
}

// ============================================================================
// detail:任务上下文与引擎核心
// ============================================================================

namespace detail {

struct EngineCore;

struct TaskContext {
  std::string task_id;
  std::string task_type;
  std::shared_ptr<BT::Tree> tree;
  std::shared_ptr<FailureLog> failures = std::make_shared<FailureLog>();
  std::atomic<TaskState> state{TaskState::Queued};
  std::atomic<bool> finished{false};
  std::mutex wait_mu;
  std::condition_variable wait_cv;
};

struct EngineCore : std::enable_shared_from_this<EngineCore> {
  BT::BehaviorTreeFactory factory;
  Scheduler scheduler;
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<TaskContext>> active;
  Engine::FinishedCallback on_finished;  // mu_ 保护
  Engine::LogCallback log = [](LogLevel, const std::string& msg) {
    std::fprintf(stderr, "[nebulabt] %s\n", msg.c_str());
  };

  void logf(LogLevel lv, const std::string& msg) {
    std::lock_guard<std::mutex> lk(mu);
    if (log) log(lv, msg);
  }

  std::shared_ptr<TaskContext> findTask(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = active.find(id);
    return it != active.end() ? it->second : nullptr;
  }

  std::shared_ptr<FailureLog> findFailures(const std::string& taskId) {
    auto ctx = findTask(taskId);
    return ctx ? ctx->failures : nullptr;
  }

  void tickTask(const std::string& id) {
    auto ctx = findTask(id);
    if (!ctx) return;

    TaskState expected = TaskState::Queued;
    ctx->state.compare_exchange_strong(expected, TaskState::Running);
    if (ctx->state.load() != TaskState::Running) {
      return; // 已终态(如已取消)
    }

    BT::NodeStatus status;
    try {
      status = ctx->tree->tickOnce();
    } catch (const std::exception& e) {
      logf(LogLevel::Error, "task " + id + " tick exception: " + e.what());
      finishTask(ctx, TaskState::Failed, e.what());
      return;
    }

    if (status == BT::NodeStatus::RUNNING) return;
    finishTask(ctx,
               status == BT::NodeStatus::SUCCESS ? TaskState::Succeeded
                                                 : TaskState::Failed,
               std::string("tree returned ") + BT::toStr(status));
  }

  void finishTask(std::shared_ptr<TaskContext> ctx, TaskState st,
                  const std::string& reason) {
    if (ctx->finished.exchange(true)) {
      return; // 只允许一次终态迁移
    }
    ctx->state.store(st);
    {
      std::lock_guard<std::mutex> lk(mu);
      active.erase(ctx->task_id);
    }
    {
      // 与 wait() 的谓词检查建立顺序,防止丢唤醒
      std::lock_guard<std::mutex> lk(ctx->wait_mu);
    }
    ctx->wait_cv.notify_all();

    logf(LogLevel::Info, "task " + ctx->task_id + " " + toString(st) +
                             (reason.empty() ? "" : " (" + reason + ")"));

    Engine::FinishedCallback cb;
    {
      std::lock_guard<std::mutex> lk(mu);
      cb = on_finished;
    }
    if (cb) {
      cb(TaskHandle(ctx, shared_from_this()));
    }
  }
};

// ============================================================================
// detail:BridgeNode —— NebulaAction 与 BT.CPP 的桥
//
// 端口声明来自 action 实例(运行期),经
//   factory.registerNodeType<BridgeNode>(id, ports, action, sched, core)
// 注册(BT.CPP 4.x 支持显式传 PortsList)。
//
// 线程模型:onStart/onRunning 只在调度线程执行;done 可在任意线程调用,
// 结果经 Slot(共享指针,生命周期独立于节点)暂存,再 post 唤醒调度器。
// ============================================================================

// result(json)中的键按端口类型写回 Output 端口
static void writeResultToOutputs(BT::TreeNode& node, const BT::PortsList& ports,
                                 const nlohmann::json& result) {
  for (const auto& kv : ports) {
    const std::string& name = kv.first;
    const BT::PortInfo& info = kv.second;
    const auto dir = info.direction();
    if (dir != BT::PortDirection::OUTPUT && dir != BT::PortDirection::INOUT) {
      continue;
    }
    if (!result.contains(name)) {
      continue;
    }
    const auto& v = result.at(name);
    try {
      const auto& t = info.type();
      if (t == typeid(std::string)) {
        node.setOutput(name, v.get<std::string>());
      } else if (t == typeid(double)) {
        node.setOutput(name, v.get<double>());
      } else if (t == typeid(int)) {
        node.setOutput(name, v.get<int>());
      } else if (t == typeid(bool)) {
        node.setOutput(name, v.get<bool>());
      } else {
        node.setOutput(name, v.dump());
      }
    } catch (const std::exception&) {
      node.setOutput(name, v.dump());
    }
  }
}

class BridgeNode : public BT::StatefulActionNode {
public:
  BridgeNode(const std::string& name, const BT::NodeConfig& config,
             NebulaActionPtr action, Scheduler* scheduler, EngineCore* core)
    : BT::StatefulActionNode(name, config), action_(std::move(action)),
      scheduler_(scheduler), core_(core), slot_(std::make_shared<Slot>()) {}

  static BT::PortsList providedPorts() { return {}; }

  BT::NodeStatus onStart() override {
    initCtx();
    BT::NodeStatus status = BT::NodeStatus::FAILURE;
    try {
      status = action_->start(*ctx_, makeDone());
    } catch (const std::exception& e) {
      ctx_->fail("ACTION_EXCEPTION", e.what());
      return BT::NodeStatus::FAILURE;
    }
    if (status != BT::NodeStatus::RUNNING) {
      // 同步完成:以返回值为准,丢弃可能已写入的 pending
      clearPending();
    }
    return status;
  }

  BT::NodeStatus onRunning() override {
    // done 已回调:应用结果并返回终态
    if (auto result = takePending()) {
      writeResultToOutputs(*this, action_->providedPorts(), result->result);
      return result->status;
    }
    // 等待 done 回调唤醒调度器
    return BT::NodeStatus::RUNNING;
  }

  void onHalted() override {
    if (ctx_) {
      try {
        action_->halt(*ctx_);
      } catch (...) {
      }
    }
    clearPending();
  }

private:
  struct PendingResult {
    BT::NodeStatus status;
    nlohmann::json result;
  };
  struct Slot {
    std::mutex mu;
    bool ready = false;
    std::unique_ptr<PendingResult> data;
  };

  std::unique_ptr<PendingResult> takePending() {
    std::lock_guard<std::mutex> lk(slot_->mu);
    if (!slot_->ready) {
      return nullptr;
    }
    slot_->ready = false;
    return std::move(slot_->data);
  }

  void clearPending() {
    std::lock_guard<std::mutex> lk(slot_->mu);
    slot_->ready = false;
    slot_->data.reset();
  }

  void initCtx() {
    std::string taskId = "unknown";
    std::string taskType;
    if (auto bb = config().blackboard) {
      bb->get("task_id", taskId);      // bool 版 get,失败保持默认值
      bb->get("task_type", taskType);
    }
    task_id_ = taskId;
    // 直接 new(NebulaCtx 构造私有,friend 本类;make_unique 的实例化
    // 上下文不继承 friend 权限,不能用于私有构造)
    ctx_.reset(new NebulaCtx(*this, core_->findFailures(taskId),
                             std::move(taskId), std::move(taskType)));
  }

  Done makeDone() {
    auto slot = slot_;
    auto core = core_->shared_from_this();
    auto taskId = task_id_;
    return [slot, core, taskId](BT::NodeStatus status, nlohmann::json result) {
      {
        std::lock_guard<std::mutex> lk(slot->mu);
        if (slot->ready) {
          return; // 重复回调,只认第一次
        }
        slot->data = std::make_unique<PendingResult>(
            PendingResult{status, std::move(result)});
        slot->ready = true;
      }
      core->scheduler.post([core, taskId] { core->tickTask(taskId); });
    };
  }

  NebulaActionPtr action_;
  Scheduler* scheduler_;
  EngineCore* core_;
  std::shared_ptr<Slot> slot_;
  std::unique_ptr<NebulaCtx> ctx_;
  std::string task_id_;
};

// explain() 的树遍历:节点路径 + 当前状态
static nlohmann::json walkNode(const BT::TreeNode* node, const std::string& path) {
  nlohmann::json j;
  j["name"] = node->name();
  j["type"] = node->registrationName();
  j["status"] = std::string(BT::toStr(node->status()));
  const std::string here =
      path.empty() ? node->name() : path + "/" + node->name();
  j["path"] = here;

  nlohmann::json children = nlohmann::json::array();
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      children.push_back(walkNode(child, here));
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      children.push_back(walkNode(dec->child(), here));
    }
  }
  j["children"] = children;
  return j;
}

static void injectSlots(BT::Tree& tree, const TaskSpec& spec) {
  auto bb = tree.rootBlackboard();
  bb->set("task_id", spec.task_id);
  bb->set("task_type", spec.task_type);
  for (auto it = spec.slots.begin(); it != spec.slots.end(); ++it) {
    const auto& v = it.value();
    if (v.is_string()) {
      bb->set(it.key(), v.get<std::string>());
    } else if (v.is_array()) {
      std::vector<std::string> vec;
      vec.reserve(v.size());
      for (const auto& e : v) {
        vec.push_back(e.is_string() ? e.get<std::string>() : e.dump());
      }
      bb->set(it.key(), vec);
    } else {
      bb->set(it.key(), v.dump());
    }
  }
}

static std::string generateTaskId() {
  static std::atomic<uint64_t> counter{0};
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return "task-" + std::to_string(ms) + "-" +
         std::to_string(counter.fetch_add(1));
}

} // namespace detail

// ============================================================================
// NebulaCtx 实现
// ============================================================================

NebulaCtx::NebulaCtx(BT::TreeNode& node, std::shared_ptr<FailureLog> failures,
                     std::string taskId, std::string taskType)
  : node_(&node), failures_(std::move(failures)),
    task_id_(std::move(taskId)), task_type_(std::move(taskType)) {}

void NebulaCtx::fail(std::string code, std::string message) {
  if (!failures_) {
    return;
  }
  FailureRecord r;
  r.code = std::move(code);
  r.node = node_->name();
  r.node_type = node_->registrationName();
  r.message = std::move(message);
  r.timestamp = nowTimestamp();
  failures_->add(std::move(r));
}

// ============================================================================
// Engine
// ============================================================================

Engine::Engine() : core_(std::make_shared<detail::EngineCore>()) {
  core_->factory.registerNodeType<ForEachNode>("ForEach");
  core_->scheduler.start();
}

Engine::~Engine() {
  core_->scheduler.stop();
}

void Engine::registerAction(const std::string& registrationId,
                            NebulaActionPtr action) {
  if (!action) {
    throw std::invalid_argument("Engine::registerAction: null action");
  }
  // 端口来自 action 实例,BT.CPP 支持注册期显式传入 PortsList
  core_->factory.registerNodeType<detail::BridgeNode>(
      registrationId, action->providedPorts(), action, &core_->scheduler,
      core_.get());
}

TaskHandle Engine::submitFromText(const std::string& xmlText, TaskSpec spec) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  std::shared_ptr<BT::Tree> tree;
  try {
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromText(xmlText));
  } catch (const std::exception& e) {
    core_->logf(LogLevel::Error, "create tree failed: " + std::string(e.what()));
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec));
}

TaskHandle Engine::submitFromFile(const std::string& xmlPath, TaskSpec spec) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  std::shared_ptr<BT::Tree> tree;
  try {
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromFile(xmlPath));
  } catch (const std::exception& e) {
    core_->logf(LogLevel::Error, "create tree failed: " + std::string(e.what()));
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec));
}

TaskHandle Engine::submitTree(std::shared_ptr<BT::Tree> tree, TaskSpec spec) {
  detail::injectSlots(*tree, spec);
  auto ctx = std::make_shared<detail::TaskContext>();
  ctx->task_id = spec.task_id;
  ctx->task_type = spec.task_type;
  ctx->tree = tree;
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    core_->active[spec.task_id] = ctx;
  }
  TaskHandle handle(ctx, core_);
  auto core = core_;
  auto id = spec.task_id;
  core_->scheduler.post([core, id] { core->tickTask(id); });
  return handle;
}

void Engine::onFinished(FinishedCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->on_finished = std::move(cb);
}

void Engine::setLogger(LogCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->log = std::move(cb);
}

// ============================================================================
// TaskHandle
// ============================================================================

TaskHandle::TaskHandle(std::shared_ptr<detail::TaskContext> ctx,
                       std::shared_ptr<detail::EngineCore> core)
  : ctx_(std::move(ctx)), core_(std::move(core)) {}

std::string TaskHandle::taskId() const {
  return ctx_ ? ctx_->task_id : std::string();
}

TaskState TaskHandle::state() const {
  return ctx_ ? ctx_->state.load() : TaskState::Failed;
}

TaskState TaskHandle::wait() {
  if (!ctx_) {
    return TaskState::Failed;
  }
  std::unique_lock<std::mutex> lk(ctx_->wait_mu);
  ctx_->wait_cv.wait(lk, [this] { return isTerminal(ctx_->state.load()); });
  return ctx_->state.load();
}

bool TaskHandle::cancel() {
  if (!ctx_ || isTerminal(ctx_->state.load())) {
    return false;
  }
  auto core = core_;
  auto ctx = ctx_;
  core->scheduler.post([core, ctx] {
    if (ctx->finished.load()) {
      return;
    }
    ctx->tree->haltTree();
    core->finishTask(ctx, TaskState::Canceled, "canceled by request");
  });
  return true;
}

std::vector<FailureRecord> TaskHandle::failures() const {
  if (ctx_ && ctx_->failures) {
    return ctx_->failures->snapshot();
  }
  return {};
}

nlohmann::json TaskHandle::explain() const {
  if (!ctx_) {
    return nlohmann::json();
  }
  nlohmann::json j;
  j["task_id"] = ctx_->task_id;
  j["task_type"] = ctx_->task_type;
  j["state"] = toString(ctx_->state.load());
  if (ctx_->tree && ctx_->tree->rootNode()) {
    j["tree"] = detail::walkNode(ctx_->tree->rootNode(), "");
  }
  j["failures"] = failures();
  return j;
}

} // namespace nebulabt
