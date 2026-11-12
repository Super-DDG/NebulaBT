#include "bridge_node.hpp"

#include "engine_core.hpp"
#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/basic_types.h>

namespace nebulabt {
namespace detail {

namespace {

// result 中的保留键 __fail_code/__fail_message → 结构化失败(M8):
// done 回调在任意线程执行时无法安全访问 ctx;携带保留键即可让桥在
// 调度线程内代记失败——异步包装器(如 LlmDecision)的校验逻辑
// 只依赖捕获副本,零 ctx 访问,天然线程安全
void extractFailKeys(NebulaCtx& ctx, nlohmann::json* result) {
  if (result == nullptr || !result->is_object()) {
    return;
  }
  auto it = result->find("__fail_code");
  if (it != result->end() && it->is_string()) {
    ctx.fail(it->get<std::string>(),
             result->value("__fail_message", std::string()));
    result->erase("__fail_code");
    result->erase("__fail_message");
  }
}

// result(json)中的键按端口类型写回 Output 端口
void writeResultToOutputs(BT::TreeNode& node, const BT::PortsList& ports,
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

} // namespace

BridgeNode::BridgeNode(const std::string& name, const BT::NodeConfig& config,
                       NebulaActionPtr action, Scheduler* scheduler,
                       EngineCore* core)
  : BT::StatefulActionNode(name, config), action_(std::move(action)),
    scheduler_(scheduler), core_(core), slot_(std::make_shared<Slot>()) {}

BT::PortsList BridgeNode::providedPorts() { return {}; }

struct BridgeNode::PendingResult {
  BT::NodeStatus status;
  nlohmann::json result;
};

struct BridgeNode::Slot {
  std::mutex mu;
  bool ready = false;
  std::unique_ptr<PendingResult> data;
};

BT::NodeStatus BridgeNode::onStart() {
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

BT::NodeStatus BridgeNode::onRunning() {
  // done 已回调:应用结果并返回终态
  if (auto result = takePending()) {
    extractFailKeys(*ctx_, &result->result);
    writeResultToOutputs(*this, action_->providedPorts(), result->result);
    if (result->status == BT::NodeStatus::FAILURE) {
      // __fail_code 已记录失败:size 变化,此处自动跳过兜底记录
      maybeAutoRecordFailure();
    }
    return result->status;
  }
  // 等待 done 回调唤醒调度器
  return BT::NodeStatus::RUNNING;
}

void BridgeNode::onHalted() {
  if (ctx_) {
    try {
      action_->halt(*ctx_);
    } catch (...) {
    }
  }
  clearPending();
}

std::unique_ptr<BridgeNode::PendingResult> BridgeNode::takePending() {
  std::lock_guard<std::mutex> lk(slot_->mu);
  if (!slot_->ready) {
    return nullptr;
  }
  slot_->ready = false;
  return std::move(slot_->data);
}

void BridgeNode::clearPending() {
  std::lock_guard<std::mutex> lk(slot_->mu);
  slot_->ready = false;
  slot_->data.reset();
}

/// 失败自动捕获:动作返回 FAILURE 却没有调用 ctx.fail() 时,
/// 框架兜底记录一条 NODE_FAILURE(失败必须可解释,不依赖作者自觉)
void BridgeNode::maybeAutoRecordFailure() {
  if (!ctx_) {
    return;
  }
  if (ctx_->failures_ && ctx_->failures_->size() != failCountAtStart_) {
    return; // 作者已显式记录
  }
  ctx_->fail("NODE_FAILURE", "action returned FAILURE without ctx.fail()");
}

void BridgeNode::initCtx() {
  std::string taskId = "unknown";
  std::string taskType;
  nlohmann::json inputs = nlohmann::json::object();
  if (auto bb = config().blackboard) {
    const bool idOk = bb->get("task_id", taskId);
    (void)bb->get("task_type", taskType);
    if (!idOk && core_) {
      // 失败记录经 findFailures(taskId) 路由:task_id 读不到 = 本任务
      // 的结构化失败全部静默丢失,必须可见而非默默降级
      core_->logf(LogLevel::Warn,
                  "BridgeNode '" + name() + "': task_id not readable from "
                  "blackboard (failure recording disabled for this task)");
    }
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
  // 注意:findFailures(taskId) 依赖 taskId 的值,而同一调用里的
  // std::move(taskId) 与它**求值顺序未指定**——gcc-12 下 move 先行,
  // findFailures 收到空串,failures_ 为 null,ctx.fail 全部静默丢失
  // (CI gcc-12 Release 抓获,DIAG failures.size=0 实证)。先取出再 move。
  auto failures = core_->findFailures(taskId);
  ctx_.reset(new NebulaCtx(*this, std::move(failures),
                           std::move(taskId), std::move(taskType),
                           core_->failure_codes));
  ctx_->inputs_ = std::move(inputs);
}

Done BridgeNode::makeDone() {
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

} // namespace detail
} // namespace nebulabt
