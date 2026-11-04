#pragma once
/// RetryWithDelay:失败重试 + 尝试间延时(M1 策略原语之一)。
///
/// BT.CPP 4.9 内置的 RetryUntilSuccessful 没有尝试间延时;
/// 对接真实系统的重试(RPC/机械臂/导航)几乎都需要退避间隔。
///
/// 语义(attempts = 总执行次数,与 BT.CPP RetryNode 一致):
/// - 子节点 SUCCESS / RUNNING -> 原样返回
/// - 子节点 FAILURE -> 剩余次数 > 0:延时 delay_ms 后重新 tick 子节点,
///   期间返回 RUNNING;次数用尽:返回 FAILURE
/// - 与 BT.CPP Timeout/装饰器自由组合

#include <behaviortree_cpp/decorator_node.h>
#include <chrono>
#include <functional>
#include <string>

namespace nebulabt {

class Scheduler;

class RetryWithDelayNode : public BT::DecoratorNode {
public:
  /// wake(task_id):由 Engine 注入,延时到期后唤醒对应任务
  using WakeFn = std::function<void(const std::string& task_id)>;

  RetryWithDelayNode(const std::string& name, const BT::NodeConfig& config,
                     Scheduler* scheduler, WakeFn wake);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  std::string taskIdFromBlackboard() const;

  Scheduler* scheduler_;
  WakeFn wake_;
  int attempts_left_ = 0;
  std::chrono::steady_clock::time_point budget_start_;
};

} // namespace nebulabt
