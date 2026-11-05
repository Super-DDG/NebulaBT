#include "nebulabt/core/engine.hpp"

#include "nebulabt/core/scheduler.hpp"
#include "nebulabt/core/schema.hpp"
#include "nebulabt/nodes/checkpoint.hpp"
#include "nebulabt/nodes/for_each.hpp"
#include "nebulabt/nodes/retry_with_delay.hpp"
#include "nebulabt/nodes/switch_n.hpp"
#include "nebulabt/nodes/while_loop.hpp"

#include <behaviortree_cpp/behavior_tree.h>
#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/decorator_node.h>
#include <behaviortree_cpp/loggers/abstract_logger.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
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

// ---- 节点计时(explain 完整版)----
// 每次 IDLE->RUNNING 计一次 attempt;RUNNING->终态 累计运行时长。
struct NodeStat {
  int attempts = 0;
  double running_ms = 0;   // 累计 RUNNING 时长(相对任务起点)
  double first_ms = -1;    // 首次状态迁移
  double last_ms = -1;     // 最近状态迁移
  double run_start_ms = 0; // 本次 RUNNING 的起点
  bool in_running = false;
};

struct NodeStatsMap {
  std::mutex mu;
  std::map<std::string, NodeStat> by_path;
};

// 订阅全树状态迁移(BT.CPP StatusChangeLogger),填充 NodeStatsMap。
// 时间戳用相对模式(相对 logger 构造时刻 ≈ 任务提交时刻)。
class TaskTraceLogger : public BT::StatusChangeLogger {
public:
  TaskTraceLogger(BT::TreeNode* root, std::shared_ptr<NodeStatsMap> stats)
    : BT::StatusChangeLogger(root), stats_(std::move(stats)) {
    setTimestampType(BT::TimestampType::relative);
  }

  void callback(BT::Duration timestamp, const BT::TreeNode& node,
                BT::NodeStatus prev, BT::NodeStatus status) override {
    const double ms =
        std::chrono::duration<double, std::milli>(timestamp).count();
    std::lock_guard<std::mutex> lk(stats_->mu);
    auto& st = stats_->by_path[node.fullPath()];
    if (st.first_ms < 0) st.first_ms = ms;
    st.last_ms = ms;

    if (status == BT::NodeStatus::RUNNING) {
      if (prev == BT::NodeStatus::IDLE) {
        st.attempts++;
      }
      st.run_start_ms = ms;
      st.in_running = true;
    } else if (status == BT::NodeStatus::SUCCESS ||
               status == BT::NodeStatus::FAILURE ||
               status == BT::NodeStatus::IDLE) {
      if (prev == BT::NodeStatus::IDLE) {
        // 同步节点:IDLE 直达终态,没有 RUNNING 迁移
        st.attempts++;
      }
      if (st.in_running) {
        st.running_ms += ms - st.run_start_ms;
        st.in_running = false;
      }
    }
  }

  void flush() override {}

private:
  std::shared_ptr<NodeStatsMap> stats_;
};

struct TaskContext {
  std::string task_id;
  std::string task_type;
  std::string pack_name;    // 来源树包(M3;直接提交 XML 时为空)
  std::string pack_version;
  std::shared_ptr<BT::Tree> tree;
  std::shared_ptr<NodeStatsMap> node_stats = std::make_shared<NodeStatsMap>();
  std::shared_ptr<BT::StatusChangeLogger> trace;  // 声明在 tree 之后:先于树析构
  std::shared_ptr<FailureLog> failures = std::make_shared<FailureLog>();
  std::atomic<TaskState> state{TaskState::Queued};
  std::atomic<bool> finished{false};
  std::mutex wait_mu;
  std::condition_variable wait_cv;

  // journal(M2):任务事件日志 + 已完成检查点的黑板快照
  static constexpr size_t kJournalMax = 1000;
  std::mutex journal_mu;
  std::vector<JournalEvent> journal;
  // 仅含从 resume_journal 种子的检查点(上一轮的完成历史)。
  // 本 run 新完成的检查点只进 journal,不进此 map——否则循环体内
  // 第二次迭代会被 tryResume 误判"已完成"而跳过工作。
  std::map<std::string, nlohmann::json> resume_checkpoints;
};

struct EngineCore : std::enable_shared_from_this<EngineCore>, ICheckpointHost {
  // ---- 树包注册表(M3)----
  struct PackEntry {
    PackManifest manifest;
    std::string dir;  // manifest 所在目录
  };
  struct Canary {
    std::string pack;
    std::string version;
    int percent = 0;
  };

  BT::BehaviorTreeFactory factory;
  Scheduler scheduler;
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<TaskContext>> active;
  std::vector<PackEntry> packs;  // mu_ 保护
  Canary canary;                 // mu_ 保护
  Engine::FinishedCallback on_finished;  // mu_ 保护
  Engine::JournalSink journal_sink;      // mu_ 保护
  Engine::LogCallback log = [](LogLevel, const std::string& msg) {
    std::fprintf(stderr, "[nebulabt] %s\n", msg.c_str());
  };

  void logf(LogLevel lv, const std::string& msg) {
    std::lock_guard<std::mutex> lk(mu);
    if (log) log(lv, msg);
  }

  // ---- journal ----
  void appendJournal(const std::shared_ptr<TaskContext>& ctx,
                     const std::string& event, nlohmann::json data) {
    JournalEvent e;
    e.timestamp = nowTimestamp();
    e.task_id = ctx->task_id;
    e.task_type = ctx->task_type;
    e.event = event;
    e.data = std::move(data);
    Engine::JournalSink sink;
    {
      std::lock_guard<std::mutex> lk(ctx->journal_mu);
      if (ctx->journal.size() < TaskContext::kJournalMax) {
        ctx->journal.push_back(e);
      }
      std::lock_guard<std::mutex> lk2(mu);
      sink = journal_sink;
    }
    if (sink) {
      sink(e);  // 锁外回调,避免死锁
    }
  }

  // ---- ICheckpointHost ----
  bool checkpointTryResume(const std::string& task_id,
                           const std::string& cp_id,
                           nlohmann::json* out_snapshot) override {
    auto ctx = findTask(task_id);
    if (!ctx) return false;
    bool found = false;
    nlohmann::json snap;
    {
      std::lock_guard<std::mutex> lk(ctx->journal_mu);
      auto it = ctx->resume_checkpoints.find(cp_id);
      if (it != ctx->resume_checkpoints.end()) {
        snap = it->second;
        found = true;
      }
    }
    if (found) {
      // 锁外记录(appendJournal 会再锁 journal_mu,不可重入)。
      // 同时补一条 "checkpoint" 事件:多跳恢复(run3 从 run2 的 journal
      // 恢复)时,完成历史得以延续,不会因只有 restored 事件而断链。
      appendJournal(ctx, "checkpoint",
                    nlohmann::json{{"id", cp_id}, {"blackboard", snap}});
      appendJournal(ctx, "checkpoint_restored", nlohmann::json{{"id", cp_id}});
    }
    return found;
  }

  void checkpointRecord(const std::string& task_id, const std::string& cp_id,
                        const nlohmann::json& snapshot) override {
    auto ctx = findTask(task_id);
    if (!ctx) return;
    // 只进 journal:本 run 的完成历史;resume 历史只在提交时种子
    appendJournal(ctx, "checkpoint",
                  nlohmann::json{{"id", cp_id}, {"blackboard", snapshot}});
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
    if (ctx->state.compare_exchange_strong(expected, TaskState::Running)) {
      appendJournal(ctx, "started", nlohmann::json::object());
    }
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

  // ---- 树包路由(M3)----
  static std::array<int, 3> parseVersion(const std::string& v) {
    std::array<int, 3> out{0, 0, 0};
    std::sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
    return out;
  }
  static bool versionLess(const std::string& a, const std::string& b) {
    return parseVersion(a) < parseVersion(b);
  }
  // FNV-1a:灰度确定性哈希
  static uint64_t hashId(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
      h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
      h *= 1099511628211ull;
    }
    return h;
  }

  // 按 task_type 选 pack 条目:显式版本 > 灰度 > 最高版本
  const PackEntry* resolveTask(const std::string& taskType,
                               const std::string& explicitVersion,
                               const std::string& taskId) {
    std::lock_guard<std::mutex> lk(mu);
    const PackEntry* best = nullptr;
    const PackEntry* canaryHit = nullptr;
    const PackEntry* pinned = nullptr;
    for (const auto& p : packs) {
      if (!p.manifest.tasks.count(taskType)) continue;
      if (!best || versionLess(best->manifest.version, p.manifest.version)) {
        best = &p;
      }
      if (canary.percent > 0 && canary.pack == p.manifest.name &&
          canary.version == p.manifest.version &&
          (hashId(taskId) % 100) < static_cast<uint64_t>(canary.percent)) {
        canaryHit = &p;
      }
      if (!explicitVersion.empty() && p.manifest.version == explicitVersion) {
        pinned = &p;
      }
    }
    if (pinned) return pinned;
    if (canaryHit) return canaryHit;
    return best;
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

    appendJournal(ctx, "finished",
                  nlohmann::json{{"state", toString(st)}, {"reason", reason}});

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
    failCountAtStart_ =
        (ctx_ && ctx_->failures_) ? ctx_->failures_->size() : 0;
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
      if (status == BT::NodeStatus::FAILURE) {
        maybeAutoRecordFailure();
      }
    }
    return status;
  }

  BT::NodeStatus onRunning() override {
    // done 已回调:应用结果并返回终态
    if (auto result = takePending()) {
      writeResultToOutputs(*this, action_->providedPorts(), result->result);
      if (result->status == BT::NodeStatus::FAILURE) {
        maybeAutoRecordFailure();
      }
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

  /// 失败自动捕获:动作返回 FAILURE 却没有调用 ctx.fail() 时,
  /// 框架兜底记录一条 NODE_FAILURE(失败必须可解释,不依赖作者自觉)
  void maybeAutoRecordFailure() {
    if (!ctx_) {
      return;
    }
    if (ctx_->failures_ && ctx_->failures_->size() != failCountAtStart_) {
      return; // 作者已显式记录
    }
    ctx_->fail("NODE_FAILURE",
               "action returned FAILURE without ctx.fail()");
  }

  void initCtx() {
    std::string taskId = "unknown";
    std::string taskType;
    nlohmann::json inputs = nlohmann::json::object();
    if (auto bb = config().blackboard) {
      bb->get("task_id", taskId);      // bool 版 get,失败保持默认值
      bb->get("task_type", taskType);
    }
    // 输入端口快照(激活时刻):{key} 引用解析黑板值,字面量保持 string
    for (const auto& [port, remap] : config().input_ports) {
      if (remap.size() >= 3 && remap.front() == '{' && remap.back() == '}') {
        const std::string key = remap.substr(1, remap.size() - 2);
        if (auto bb = config().blackboard) {
          if (auto entry = bb->getAnyLocked(key)) {
            inputs[port] = nebulabt::anyToJson(entry);
            continue;
          }
        }
        inputs[port] = nullptr;  // 引用的黑板键尚无值
      } else {
        inputs[port] = remap;    // 字面量
      }
    }
    task_id_ = taskId;
    // 直接 new(NebulaCtx 构造私有,friend 本类;make_unique 的实例化
    // 上下文不继承 friend 权限,不能用于私有构造)
    ctx_.reset(new NebulaCtx(*this, core_->findFailures(taskId),
                             std::move(taskId), std::move(taskType)));
    ctx_->inputs_ = std::move(inputs);
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
  size_t failCountAtStart_ = 0;
};

// explain() 的树遍历:节点路径 + 当前状态 + 计时(attempts / running_ms)
static nlohmann::json walkNode(const BT::TreeNode* node, const std::string& path,
                               detail::NodeStatsMap* stats) {
  nlohmann::json j;
  j["name"] = node->name();
  j["type"] = node->registrationName();
  j["status"] = std::string(BT::toStr(node->status()));
  const std::string here =
      path.empty() ? node->name() : path + "/" + node->name();
  j["path"] = here;

  if (stats) {
    std::lock_guard<std::mutex> lk(stats->mu);
    auto it = stats->by_path.find(node->fullPath());
    if (it != stats->by_path.end()) {
      const auto& st = it->second;
      j["attempts"] = st.attempts;
      j["running_ms"] = std::round(st.running_ms * 10.0) / 10.0;
      j["first_ms"] = std::round(st.first_ms * 10.0) / 10.0;
      j["last_ms"] = std::round(st.last_ms * 10.0) / 10.0;
    }
  }

  nlohmann::json children = nlohmann::json::array();
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      children.push_back(walkNode(child, here, stats));
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      children.push_back(walkNode(dec->child(), here, stats));
    }
  }
  j["children"] = children;
  return j;
}

// ============================================================================
// L2 校验:树 ↔ schema 的端口级数据流检查
//
// 收集树中所有端口引用的黑板键({key} 形式),要求每个"被读"的键属于:
//   schema 声明 / 框架注入(task_id, task_type)/ 树内某节点 Output 写出
// 目标:键名拼写错误、忘记声明 slots 这类问题在加载期暴露,
// 而不是运行时表现为"注入了但没人读"的空串。
// ============================================================================

struct KeyRef {
  std::string key;
  std::string node;
  std::string port;
  bool is_write;
};

static bool parseKeyRef(const std::string& remap, std::string* keyOut) {
  if (remap.size() < 3 || remap.front() != '{' || remap.back() != '}') {
    return false;
  }
  std::string key = remap.substr(1, remap.size() - 2);
  // 去空白
  const auto first = key.find_first_not_of(" \t");
  const auto last = key.find_last_not_of(" \t");
  if (first == std::string::npos) {
    return false;
  }
  *keyOut = key.substr(first, last - first + 1);
  return !keyOut->empty();
}

static void collectKeyRefs(const BT::TreeNode* node, std::vector<KeyRef>& refs) {
  for (const auto& [port, remap] : node->config().input_ports) {
    std::string key;
    if (parseKeyRef(remap, &key)) {
      refs.push_back(KeyRef{key, node->name(), port, false});
    }
  }
  for (const auto& [port, remap] : node->config().output_ports) {
    std::string key;
    if (parseKeyRef(remap, &key)) {
      refs.push_back(KeyRef{key, node->name(), port, true});
    }
  }
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      collectKeyRefs(child, refs);
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      collectKeyRefs(dec->child(), refs);
    }
  }
}

static void validateTreeKeys(const BT::Tree& tree,
                             const std::map<std::string, SlotDef>& defs,
                             std::vector<std::string>& errors) {
  std::vector<KeyRef> refs;
  collectKeyRefs(tree.rootNode(), refs);

  std::set<std::string> produced{"task_id", "task_type"};  // 框架注入
  for (const auto& [key, def] : defs) {
    produced.insert(key);
  }
  for (const auto& ref : refs) {
    if (ref.is_write) {
      produced.insert(ref.key);
    }
  }
  for (const auto& ref : refs) {
    if (!ref.is_write && !produced.count(ref.key)) {
      errors.push_back("node '" + ref.node + "' port '" + ref.port +
                       "' reads undeclared blackboard key '" + ref.key +
                       "' (not in schema and never written by the tree)");
    }
  }
}

static void injectSlots(BT::Tree& tree, const TaskSpec& spec,
                        const std::map<std::string, SlotDef>& defs) {
  auto bb = tree.rootBlackboard();
  bb->set("task_id", spec.task_id);
  bb->set("task_type", spec.task_type);
  for (auto it = spec.slots.begin(); it != spec.slots.end(); ++it) {
    const auto& v = it.value();
    // schema 声明的标量类型按真实类型注入(类型已在 L1 校验通过)
    auto defIt = defs.find(it.key());
    if (defIt != defs.end()) {
      const auto& type = defIt->second.type;
      if (type == "int" && v.is_number_integer()) {
        bb->set(it.key(), v.get<int>());
        continue;
      }
      if (type == "double" && v.is_number()) {
        bb->set(it.key(), v.get<double>());
        continue;
      }
      if (type == "bool" && v.is_boolean()) {
        bb->set(it.key(), v.get<bool>());
        continue;
      }
    }
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
  // 内置动态树原语与策略节点
  core_->factory.registerNodeType<ForEachNode>("ForEach");
  core_->factory.registerNodeType<WhileLoopNode>("WhileLoop");
  core_->factory.registerNodeType<SwitchNNode>("SwitchN");

  // RetryWithDelay 需要延时唤醒:注入调度器与任务唤醒函数
  auto core = core_;
  core_->factory.registerNodeType<RetryWithDelayNode>(
      "RetryWithDelay", &core_->scheduler,
      [core](const std::string& taskId) {
        core->scheduler.post([core, taskId] { core->tickTask(taskId); });
      });

  // Checkpoint 需要读写任务的检查点历史与 journal:注入宿主(EngineCore)
  core_->factory.registerNodeType<CheckpointNode>("Checkpoint",
                                                  core_.get());

  core_->scheduler.start();
}

Engine::~Engine() {
  // 取消全部活动任务(halt + CANCELED),再等调度器把取消作业执行完——
  // 持有句柄的 wait() 会正常返回,不再永久挂起
  std::vector<std::shared_ptr<detail::TaskContext>> toCancel;
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (auto& [id, ctx] : core_->active) {
      toCancel.push_back(ctx);
    }
  }
  for (auto& ctx : toCancel) {
    core_->scheduler.post([core = core_, ctx] {
      if (ctx->finished.load()) {
        return;
      }
      ctx->tree->haltTree();
      core->finishTask(ctx, TaskState::Canceled, "engine destroyed");
    });
  }
  core_->scheduler.drainAndStop();
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
  if (!validateTask(*tree, spec)) {
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec));
}

TaskHandle Engine::submitFromFile(const std::string& xmlPath, TaskSpec spec) {
  return submitFromFile(xmlPath, std::move(spec), "", "");
}

TaskHandle Engine::submitFromFile(const std::string& xmlPath, TaskSpec spec,
                                  const std::string& packName,
                                  const std::string& packVersion) {
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
  if (!validateTask(*tree, spec)) {
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec), packName, packVersion);
}

void Engine::loadPack(const std::string& manifestDir) {
  const std::string manifestPath = manifestDir + "/pack.json";
  std::ifstream f(manifestPath);
  if (!f) {
    throw std::runtime_error("loadPack: cannot read " + manifestPath);
  }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    throw std::runtime_error("loadPack: manifest parse: " +
                             std::string(e.what()));
  }
  auto manifest = parsePackManifest(j);

  // requires:依赖 pack 必须已加载且版本达标(拆包有依赖顺序保障)
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (const auto& req : manifest.requires) {
      bool satisfied = false;
      for (const auto& p : core_->packs) {
        if (p.manifest.name != req.pack) continue;
        if (req.min_version.empty() ||
            !detail::EngineCore::versionLess(p.manifest.version,
                                             req.min_version)) {
          satisfied = true;
          break;
        }
      }
      if (!satisfied) {
        throw std::runtime_error(
            "loadPack[" + manifest.name + "]: requires pack '" + req.pack +
            "' >= " + (req.min_version.empty() ? "any" : req.min_version) +
            " (load dependencies first)");
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (const auto& p : core_->packs) {
      if (p.manifest.name == manifest.name &&
          p.manifest.version == manifest.version) {
        throw std::runtime_error("loadPack: pack '" + manifest.name + "' v" +
                                 manifest.version + " already loaded");
      }
    }
  }

  // 加载即校验:每棵树可解析、schema 数据流通过——CI 加载全部 pack 即门禁
  for (const auto& [taskType, task] : manifest.tasks) {
    const std::string path = manifestDir + "/" + manifest.trees_dir + "/" +
                             task.tree + ".xml";
    std::shared_ptr<BT::Tree> tree;
    try {
      tree = std::make_shared<BT::Tree>(core_->factory.createTreeFromFile(path));
    } catch (const std::exception& e) {
      throw std::runtime_error("loadPack[" + manifest.name + " v" +
                               manifest.version + "] task '" + taskType +
                               "': " + e.what());
    }
    std::vector<std::string> errors;
    const auto defs = parseSlotSchema(task.schema, errors);
    detail::validateTreeKeys(*tree, defs, errors);
    if (!errors.empty()) {
      std::string joined;
      for (const auto& e : errors) joined += "\n  - " + e;
      throw std::runtime_error("loadPack[" + manifest.name + " v" +
                               manifest.version + "] task '" + taskType +
                               "':" + joined);
    }
  }

  {
    std::lock_guard<std::mutex> lk(core_->mu);
    core_->packs.push_back(detail::EngineCore::PackEntry{
        std::move(manifest), manifestDir});
  }
  core_->logf(LogLevel::Info, "pack loaded (see journal for details)");
}

TaskHandle Engine::submitTask(TaskSpec spec) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  // 恢复重跑自动钉住原 pack 版本:灰度翻转时,恢复必须与原执行版本一致,
  // 否则检查点 id 对不上会导致已完成段被静默重执行
  if (!spec.resume_journal.is_array() || spec.resume_journal.empty()) {
    // 非恢复提交:不处理
  } else if (spec.pack_version.empty()) {
    for (const auto& e : spec.resume_journal) {
      if (e.is_object() && e.value("event", "") == "submitted" &&
          e.contains("data") && e["data"].value("pack_version", "").size() > 0) {
        spec.pack_version = e["data"]["pack_version"].get<std::string>();
        break;
      }
    }
  }
  const detail::EngineCore::PackEntry* entry =
      core_->resolveTask(spec.task_type, spec.pack_version, spec.task_id);
  if (!entry) {
    core_->logf(LogLevel::Error,
                "submitTask: no loaded pack provides task_type '" +
                    spec.task_type + "'");
    return TaskHandle();
  }
  const auto& task = entry->manifest.tasks.at(spec.task_type);
  if (spec.schema.empty()) {
    spec.schema = task.schema;
  }
  if (spec.timeout_ms == 0) {
    spec.timeout_ms = task.timeout_ms;
  }
  // slot_defaults:提交未给的键用 pack 声明的默认值(变体不 fork 的机制)
  for (auto it = task.slot_defaults.begin(); it != task.slot_defaults.end();
       ++it) {
    if (!spec.slots.contains(it.key())) {
      spec.slots[it.key()] = it.value();
    }
  }
  const std::string path = entry->dir + "/" + entry->manifest.trees_dir +
                           "/" + task.tree + ".xml";
  return submitFromFile(path, std::move(spec), entry->manifest.name,
                        entry->manifest.version);
}

void Engine::setCanary(const std::string& packName, const std::string& version,
                       int percent) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->canary = {packName, version, percent};
}

bool Engine::validateTask(BT::Tree& tree, const TaskSpec& spec) const {
  std::vector<std::string> errors;
  const auto defs = parseSlotSchema(spec.schema, errors);
  auto slotErrors = validateSlotsAgainstSchema(defs, spec.slots);
  errors.insert(errors.end(), slotErrors.begin(), slotErrors.end());
  if (!defs.empty()) {
    // schema 未启用时跳过 L2 数据流检查(向后兼容既有任务)
    detail::validateTreeKeys(tree, defs, errors);
  }
  if (errors.empty()) {
    return true;
  }
  for (const auto& e : errors) {
    core_->logf(LogLevel::Error, "validation: " + e);
  }
  return false;
}

std::vector<std::string> Engine::validate(const std::string& xmlText,
                                          const nlohmann::json& schema) {
  std::vector<std::string> errors;
  std::shared_ptr<BT::Tree> tree;
  try {
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromText(xmlText));
  } catch (const std::exception& e) {
    errors.push_back(std::string("parse error: ") + e.what());
    return errors;
  }
  const auto defs = parseSlotSchema(schema, errors);
  detail::validateTreeKeys(*tree, defs, errors);
  return errors;
}

TaskHandle Engine::submitTree(std::shared_ptr<BT::Tree> tree, TaskSpec spec,
                              const std::string& packName,
                              const std::string& packVersion) {
  std::vector<std::string> parseErrors;  // schema 格式问题已在 validateTask 上报
  const auto defs = parseSlotSchema(spec.schema, parseErrors);
  detail::injectSlots(*tree, spec, defs);
  auto ctx = std::make_shared<detail::TaskContext>();
  ctx->task_id = spec.task_id;
  ctx->task_type = spec.task_type;
  ctx->pack_name = packName;
  ctx->pack_version = packVersion;
  ctx->tree = tree;
  // 节点计时:订阅全树状态迁移(相对任务提交时刻)
  ctx->trace = std::make_shared<detail::TaskTraceLogger>(tree->rootNode(),
                                                         ctx->node_stats);
  {
    // 断点恢复(M2):从上一轮 journal 提取已完成的检查点快照
    std::lock_guard<std::mutex> lk(ctx->journal_mu);
    if (spec.resume_journal.is_array()) {
      for (const auto& e : spec.resume_journal) {
        JournalEvent ev;
        try {
          ev = e.get<JournalEvent>();
        } catch (const std::exception&) {
          continue;
        }
        if (ev.event == "checkpoint" && ev.data.contains("id") &&
            ev.data.contains("blackboard")) {
          ctx->resume_checkpoints[ev.data["id"].get<std::string>()] =
              ev.data["blackboard"];
        }
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    core_->active[spec.task_id] = ctx;
  }
  TaskHandle handle(ctx, core_);
  core_->appendJournal(
      ctx, "submitted",
      nlohmann::json{{"task_type", spec.task_type},
                     {"pack", packName},
                     {"pack_version", packVersion},
                     {"resumed",
                      spec.resume_journal.is_array() &&
                          !spec.resume_journal.empty()}});
  auto core = core_;
  auto id = spec.task_id;
  core_->scheduler.post([core, id] { core->tickTask(id); });

  // 策略:任务级超时(调度线程回调;到点未终态则 halt + TASK_TIMEOUT)
  if (spec.timeout_ms > 0) {
    const uint64_t timeoutMs = spec.timeout_ms;
    core_->scheduler.callLater(std::chrono::milliseconds(timeoutMs),
                               [core, ctx, timeoutMs] {
      if (ctx->finished.load()) {
        return;
      }
      FailureRecord fr;
      fr.code = "TASK_TIMEOUT";
      fr.node = "engine";
      fr.node_type = "Engine";
      fr.message = "task exceeded " + std::to_string(timeoutMs) + " ms";
      fr.timestamp = nowTimestamp();
      ctx->failures->add(std::move(fr));
      ctx->tree->haltTree();
      core->finishTask(ctx, TaskState::Failed, "task timeout");
    });
  }
  return handle;
}

void Engine::onFinished(FinishedCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->on_finished = std::move(cb);
}

void Engine::setJournalSink(JournalSink cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->journal_sink = std::move(cb);
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

TaskState TaskHandle::waitFor(std::chrono::milliseconds timeout) const {
  if (!ctx_) {
    return TaskState::Failed;
  }
  std::unique_lock<std::mutex> lk(ctx_->wait_mu);
  ctx_->wait_cv.wait_for(lk, timeout,
                         [this] { return isTerminal(ctx_->state.load()); });
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

std::vector<JournalEvent> TaskHandle::journal() const {
  if (!ctx_) {
    return {};
  }
  std::lock_guard<std::mutex> lk(ctx_->journal_mu);
  return ctx_->journal;
}

nlohmann::json TaskHandle::explain() const {
  if (!ctx_) {
    return nlohmann::json();
  }
  nlohmann::json j;
  j["task_id"] = ctx_->task_id;
  j["task_type"] = ctx_->task_type;
  if (!ctx_->pack_name.empty()) {
    j["pack"] = ctx_->pack_name;
    j["pack_version"] = ctx_->pack_version;
  }
  j["state"] = toString(ctx_->state.load());
  if (ctx_->tree && ctx_->tree->rootNode()) {
    j["tree"] = detail::walkNode(ctx_->tree->rootNode(), "",
                                 ctx_->node_stats.get());
  }
  j["failures"] = failures();
  return j;
}

} // namespace nebulabt
