#include "nebulabt/nodes/retry_with_delay.hpp"

#include "nebulabt/core/scheduler.hpp"

#include <chrono>

namespace nebulabt {

RetryWithDelayNode::RetryWithDelayNode(const std::string& name,
                                       const BT::NodeConfig& config,
                                       Scheduler* scheduler, WakeFn wake)
  : BT::DecoratorNode(name, config), scheduler_(scheduler),
    wake_(std::move(wake)) {}

BT::PortsList RetryWithDelayNode::providedPorts() {
  return {
    BT::InputPort<int>("attempts", 1,
        "总执行次数上限(含首次;子节点每次失败消耗一次)"),
    BT::InputPort<int>("delay_ms", 0, "两次尝试之间的延时(毫秒)"),
  };
}

std::string RetryWithDelayNode::taskIdFromBlackboard() const {
  std::string taskId;
  if (auto bb = config().blackboard) {
    bb->get("task_id", taskId);
  }
  return taskId;
}

BT::NodeStatus RetryWithDelayNode::tick() {
  if (status() == BT::NodeStatus::IDLE) {
    attempts_left_ = getInput<int>("attempts").value_or(1);
  }

  const BT::NodeStatus childStatus = child()->executeTick();
  if (childStatus != BT::NodeStatus::FAILURE) {
    return childStatus;  // SUCCESS / RUNNING 原样返回
  }

  // 子节点失败:还有次数 -> 延时后重试;否则失败上抛
  --attempts_left_;
  if (attempts_left_ <= 0) {
    resetChild();
    return BT::NodeStatus::FAILURE;
  }

  const int delayMs = getInput<int>("delay_ms").value_or(0);
  auto wake = wake_;
  auto taskId = taskIdFromBlackboard();
  scheduler_->callLater(std::chrono::milliseconds(delayMs),
                        [wake, taskId] { wake(taskId); });
  return BT::NodeStatus::RUNNING;
}

void RetryWithDelayNode::halt() {
  attempts_left_ = 0;
  DecoratorNode::halt();
}

} // namespace nebulabt
