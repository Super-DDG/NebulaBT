#pragma once
/// 故障注入 kit(M6):不改生产动作代码,在注册层包装注入故障。
///
///   // 生产动作原样;测试注册时包一层:
///   engine.registerAction("ChargePayment",
///       testkit::flaky(realAction, /*fail_first=*/2));      // 前 2 次失败
///   engine.registerAction("NavigateTo",
///       testkit::flaky(realAction, 1, "NAV_TIMEOUT", "no route"));
///
/// 与 RetryWithDelay/Checkpoint/虚拟时钟组合,可在 CI 中复现
/// "重试中途崩溃 → 恢复重跑"的任意交错(见 test_property_recovery)。

#include "nebulabt/sdk/action.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace nebulabt::testkit {

/// 包装器:前 fail_first 次激活返回 FAILURE(记注入失败码),之后透传
/// 内层动作。取消传播到内层;调用计数跨 Engine 存活(同一实例可复用
/// 于"崩溃恢复"多轮提交,模拟故障横跨多轮)。
class FlakyAction : public NebulaAction {
public:
  FlakyAction(NebulaActionPtr inner, int fail_first,
              std::string code = "INJECTED_FAILURE",
              std::string msg = "injected fault")
    : inner_(std::move(inner)), fail_first_(fail_first),
      code_(std::move(code)), msg_(std::move(msg)) {}

  BT::PortsList providedPorts() const override {
    return inner_->providedPorts();
  }

  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    if (calls_.fetch_add(1) < fail_first_) {
      ctx.fail(code_, msg_);
      return BT::NodeStatus::FAILURE;
    }
    return inner_->start(ctx, done);
  }

  void halt(NebulaCtx& ctx) override { inner_->halt(ctx); }

private:
  NebulaActionPtr inner_;
  int fail_first_;
  std::string code_;
  std::string msg_;
  std::atomic<int> calls_{0};
};

/// 便捷工厂:fail_first < 0 表示只透传不注入(便于参数化轮次)
inline std::shared_ptr<NebulaAction> flaky(NebulaActionPtr inner,
                                           int fail_first,
                                           std::string code = "INJECTED_FAILURE",
                                           std::string msg = "injected fault") {
  return std::make_shared<FlakyAction>(std::move(inner), fail_first,
                                       std::move(code), std::move(msg));
}

} // namespace nebulabt::testkit
