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
#include <set>
#include <string>
#include <vector>

namespace nebulabt {

class LlmDecision : public NebulaAction {
public:
  /// LLM 客户端能力接口:发起调用后,在任意线程经 done 返回结果
  using Client = std::function<void(const nlohmann::json& request, Done done)>;

  explicit LlmDecision(Client client) : client_(std::move(client)) {}

  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<std::string>("prompt", "决策上下文(自然语言或结构化文本)"),
      BT::InputPort<std::string>("branches", "允许的分支,逗号分隔,如 patrol,engage"),
      BT::OutputPort<std::string>("branch", "LLM 选中的分支"),
      BT::OutputPort<std::string>("reason", "LLM 给出的理由(可选透传)"),
    };
  }

  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    std::vector<std::string> branches = splitBranches(ctx.in<std::string>("branches"));
    if (branches.empty()) {
      ctx.fail("LLM_NO_BRANCHES", "branches port is empty");
      return BT::NodeStatus::FAILURE;
    }

    const nlohmann::json request = {
      {"context", ctx.in<std::string>("prompt")},
      {"branches", branches},
    };

    // 客户端在任意线程调 done;校验只依赖捕获副本(allowed/done),
    // 零 ctx 访问——线程安全,节点销毁也无悬挂
    const std::set<std::string> allowed(branches.begin(), branches.end());
    client_(request, [allowed, done](BT::NodeStatus status,
                                     nlohmann::json result) {
      if (status != BT::NodeStatus::SUCCESS) {
        done(status, std::move(result));  // 客户端失败码原样透传
        return;
      }
      const std::string branch =
          result.is_object() ? result.value("branch", "") : "";
      if (branch.empty() || !allowed.count(branch)) {
        // 保留键约定:桥在调度线程内代记结构化失败
        done(BT::NodeStatus::FAILURE,
             nlohmann::json{{"__fail_code", "LLM_INVALID_BRANCH"},
                            {"__fail_message",
                             "branch '" + branch +
                                 "' not in allowed set (hallucination?)"},
                            {"branch", branch}});
        return;
      }
      done(BT::NodeStatus::SUCCESS, std::move(result));
    });
    return BT::NodeStatus::RUNNING;
  }

private:
  static std::vector<std::string> splitBranches(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : csv) {
      if (c == ',') {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
      } else if (c != ' ' && c != '\t') {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
  }

  Client client_;
};

} // namespace nebulabt
