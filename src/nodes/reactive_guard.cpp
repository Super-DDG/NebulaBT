#include "nebulabt/nodes/reactive_guard.hpp"

#include "nebulabt/core/any_json.hpp"
#include <behaviortree_cpp/blackboard.h>

namespace nebulabt {

ReactiveGuardNode::ReactiveGuardNode(const std::string& name,
                                     const BT::NodeConfig& config,
                                     Scheduler* scheduler,
                                     std::function<void(const std::string&)> wake,
                                     IGuardHost* host)
  : BT::DecoratorNode(name, config), scheduler_(scheduler),
    wake_(std::move(wake)), host_(host) {}

BT::PortsList ReactiveGuardNode::providedPorts() {
  return {
    // 无类型端口(只读原始 remap):若声明为 InputPort<std::string>,
    // BT.CPP 会把 {key} 绑定的黑板键钉成 string 类型,与 bool 值写入冲突
    BT::InputPort("while_true",
        "守护条件:黑板 bool 键,如 {safe};false/缺失/类型不符 = 违反(fail-closed)"),
    BT::InputPort<int>("period_ms", 100, "巡检周期(毫秒)"),
  };
}

std::string ReactiveGuardNode::taskIdFromBlackboard() const {
  std::string taskId;
  if (auto bb = config().blackboard) {
    bb->get("task_id", taskId);
  }
  return taskId;
}

bool ReactiveGuardNode::guardHolds() const {
  if (guard_key_.empty()) {
    return false;
  }
  auto bb = config().blackboard;
  if (!bb) {
    return false;
  }
  // 不能用 bb->get<T> 逐类型试探:类型不符会抛 Any::convert
  // (checkpoint 快照同款坑);走 Any 分发
  auto entry = bb->getAnyLocked(guard_key_);
  if (!entry) {
    return false;  // 键缺失:fail-closed
  }
  const nlohmann::json v = nebulabt::anyToJson(entry);
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_string()) {
    // 无 schema 提交时 bool slots 会被 dump 成字符串:兼容该形态
    return v.get<std::string>() == "true";
  }
  return false;  // 其它类型:fail-closed
}

void ReactiveGuardNode::armInspection() {
  if (inspection_armed_) {
    return;  // 只允许一条巡检链(定时器到期 -> 重 tick -> 再武装)
  }
  inspection_armed_ = true;
  const std::string taskId = taskIdFromBlackboard();
  // 定时器只捕获 task_id,不捕获 this:任务结束后定时器触发,
  // wake -> tickTask 查不到活动任务,no-op,无悬挂指针
  scheduler_->callLater(std::chrono::milliseconds(period_ms_),
                        [wake = wake_, taskId] { wake(taskId); });
}

BT::NodeStatus ReactiveGuardNode::tick() {
  if (status() == BT::NodeStatus::IDLE) {
    // 注意:getInput<std::string> 会把 {key} 解引用成黑板值;键名必须读
    // 原始 remap 串(与 BridgeNode::initCtx 同法)
    auto it = config().input_ports.find("while_true");
    if (it != config().input_ports.end() && it->second.size() >= 3 &&
        it->second.front() == '{' && it->second.back() == '}') {
      guard_key_ = it->second.substr(1, it->second.size() - 2);
    }
    if (guard_key_.empty()) {
      throw BT::RuntimeError(
          "ReactiveGuard: 'while_true' must be a blackboard key like {safe}");
    }
    period_ms_ = getInput<int>("period_ms").value_or(100);
    if (period_ms_ <= 0) {
      period_ms_ = 1;
    }
  }
  inspection_armed_ = false;  // 本 tick 重新决定是否武装

  if (!guardHolds()) {
    if (child()->status() == BT::NodeStatus::RUNNING) {
      haltChild();  // 守护破坏:打断在途工作(动作收到 halt 回调)
    }
    if (host_) {
      host_->guardViolated(taskIdFromBlackboard(), name(), guard_key_);
    }
    return BT::NodeStatus::FAILURE;
  }

  // 守护成立:照常 tick 子节点——重 tick 是异步动作应用 done 结果的
  // 唯一通道(漏掉会导致任务永久卡在 RUNNING,M7 修复的回归测试覆盖)
  const BT::NodeStatus st = child()->executeTick();
  if (st == BT::NodeStatus::RUNNING) {
    armInspection();
  }
  return st;
}

void ReactiveGuardNode::halt() {
  inspection_armed_ = false;
  DecoratorNode::halt();
}

} // namespace nebulabt
