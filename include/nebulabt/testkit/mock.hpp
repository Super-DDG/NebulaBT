#pragma once
/// 测试替身(M3):ScriptedAction —— 树级测试的 Mock 原语。
///
/// 业务动作按脚本返回,并录制每次激活的输入端口快照:
///
///   auto nav = std::make_shared<testkit::ScriptedAction>(ports);
///   nav->thenFail("NAV_TIMEOUT", "no route")   // 第 1 次:失败
///       .then(BT::NodeStatus::SUCCESS, {{"duration", 1.5}})  // 第 2 次:成功
///       .always(BT::NodeStatus::SUCCESS);      // 脚本耗尽后的兜底
///   engine.registerAction("NavigateTo", nav);
///
///   // 跑树后断言:
///   CHECK(nav->calls().size() == 2);
///   CHECK(nav->calls()[0]["point_id"] == "kitchen");
///
/// 与 RetryWithDelay/Checkpoint 组合即可覆盖金路径、失败兜底、
/// 恢复重跑等树级行为——全部在 CI 中,无需真机。

#include "nebulabt/sdk/action.hpp"
#include <atomic>
#include <mutex>
#include <vector>

namespace nebulabt::testkit {

class ScriptedAction : public NebulaAction {
public:
  explicit ScriptedAction(BT::PortsList ports) : ports_(std::move(ports)) {}

  /// 追加一个脚本步骤(按调用顺序消费)
  ScriptedAction& then(BT::NodeStatus status, nlohmann::json out = {}) {
    Step s;
    s.status = status;
    s.out = std::move(out);
    std::lock_guard<std::mutex> lk(mu_);
    steps_.push_back(std::move(s));
    return *this;
  }

  /// 追加一个失败步骤(带结构化失败码)
  ScriptedAction& thenFail(std::string code, std::string msg,
                           nlohmann::json out = {}) {
    Step s;
    s.status = BT::NodeStatus::FAILURE;
    s.out = std::move(out);
    s.fail_code = std::move(code);
    s.fail_msg = std::move(msg);
    std::lock_guard<std::mutex> lk(mu_);
    steps_.push_back(std::move(s));
    return *this;
  }

  /// 脚本耗尽后的兜底步骤(默认 SUCCESS)
  ScriptedAction& always(BT::NodeStatus status, nlohmann::json out = {}) {
    std::lock_guard<std::mutex> lk(mu_);
    always_.status = status;
    always_.out = std::move(out);
    has_always_ = true;
    return *this;
  }

  /// 每次激活的输入端口快照(按调用顺序)
  std::vector<nlohmann::json> calls() const {
    std::lock_guard<std::mutex> lk(mu_);
    return calls_;
  }

  BT::PortsList providedPorts() const override { return ports_; }

  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    Step step;
    {
      std::lock_guard<std::mutex> lk(mu_);
      calls_.push_back(ctx.inputsJson());
      if (next_ < steps_.size()) {
        step = steps_[next_++];
      } else {
        step = always_;  // has_always_=false 时 status 默认 SUCCESS
      }
    }
    if (!step.fail_code.empty()) {
      ctx.fail(step.fail_code, step.fail_msg);
    }
    if (!step.out.is_null()) {
      done(step.status, step.out);  // 输出经桥的类型分发写回端口
    }
    return step.status;  // 同步返回;桥以返回值为准
  }

private:
  struct Step {
    BT::NodeStatus status = BT::NodeStatus::SUCCESS;
    nlohmann::json out;
    std::string fail_code;
    std::string fail_msg;
  };

  BT::PortsList ports_;
  mutable std::mutex mu_;
  std::vector<Step> steps_;
  Step always_;
  bool has_always_ = false;
  size_t next_ = 0;
  std::vector<nlohmann::json> calls_;
};

} // namespace nebulabt::testkit
