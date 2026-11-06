#pragma once
/// LLMDecision:LLM 决策节点(M8,Agent 骨架)。
///
/// 设计立场:**LLM 只做"选分支",控制流仍在树里**——
///   <LLMDecision prompt="{situation}" branches="patrol,engage,retreat"
///                branch="{decision}" reason="{why}"/>
///   <SwitchN variable="{decision}" case_1="engage">...</SwitchN>
///
/// - 分支集合即 schema:输出不在集合内 → LLM_INVALID_BRANCH 失败
///   (幻觉被拦在节点边界,污染不了控制流)
/// - LLM 客户端经能力接口注入(核心库零 HTTP 依赖):
///
///   nebulabt::LlmDecision::Client client =
///       [](const nlohmann::json& request, Done done) {
///         // 你的 HTTP/SDK 调用;任意线程调 done
///         llm.complete(request.dump(), [done](std::string text) {
///           done(BT::NodeStatus::SUCCESS, nlohmann::json::parse(text));
///         });
///       };
///   engine.registerAction("LLMDecision",
///                         std::make_shared<LlmDecision>(client));
///
/// - request 格式:{"context": <prompt 端口>, "branches": [...]}
///   期望结果:{"branch": "...", "reason": "..."}(reason 可选)
/// - 失败码:LLM_NO_BRANCHES / LLM_INVALID_BRANCH / 客户端异常走
///   ACTION_EXCEPTION;建议 Engine::declareFailureCodes 登记
///
/// 与 Checkpoint/ReactiveGuard/RetryWithDelay 组合即 agent 骨架:
/// 决策可恢复、可守护、可重试,explain 就是完整执行轨迹。

#include "nebulabt/sdk/action.hpp"
#include <functional>
#include <string>
#include <vector>

namespace nebulabt {

class LlmDecision : public NebulaAction {
public:
  /// LLM 客户端能力接口:发起调用后,在任意线程经 done 返回结果
  using Client = std::function<void(const nlohmann::json& request, Done done)>;

  explicit LlmDecision(Client client);

  BT::PortsList providedPorts() const override;

  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override;

private:
  static std::vector<std::string> splitBranches(const std::string& csv);

  Client client_;
};

} // namespace nebulabt
