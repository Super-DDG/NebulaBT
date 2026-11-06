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
    BT::InputPort<int>("budget_ms", 0,
        "重试墙钟预算(毫秒),0 = 不限;次数与时间任一用尽即失败"),
  };
}

std::string RetryWithDelayNode::taskIdFromBlackboard() const {
  std::string taskId;
  if (auto bb = config().blackboard) {
    (void)bb->get("task_id", taskId);
  }
  return taskId;
}

BT::NodeStatus RetryWithDelayNode::tick() {
  if (status() == BT::NodeStatus::IDLE) {
    attempts_left_ = getInput<int>("attempts").value_or(1);
    budget_start_ = std::chrono::steady_clock::now();
  }

  const BT::NodeStatus childStatus = child()->executeTick();
  if (childStatus != BT::NodeStatus::FAILURE) {
    return childStatus;  // SUCCESS / RUNNING 原样返回
  }

  // 子节点失败:次数与墙钟预算任一用尽即失败上抛
  --attempts_left_;
  const bool attemptsLeft = attempts_left_ > 0;

  bool budgetLeft = true;
  const int budgetMs = getInput<int>("budget_ms").value_or(0);
  if (budgetMs > 0) {
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - budget_start_)
                               .count();
    budgetLeft = elapsedMs < budgetMs;
  }

  if (!attemptsLeft || !budgetLeft) {
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
