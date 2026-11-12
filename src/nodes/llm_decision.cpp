#include "nebulabt/nodes/llm_decision.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

namespace nebulabt {

LlmDecision::LlmDecision(Client client) : client_(std::move(client)) {}

BT::PortsList LlmDecision::providedPorts() const {
  return {
    BT::InputPort<std::string>("prompt", "决策上下文(自然语言或结构化文本)"),
    BT::InputPort<std::string>("branches", "允许的分支,逗号分隔,如 patrol,engage"),
    BT::OutputPort<std::string>("branch", "LLM 选中的分支"),
    BT::OutputPort<std::string>("reason", "LLM 给出的理由(可选透传)"),
  };
}

std::vector<std::string> LlmDecision::splitBranches(const std::string& csv) {
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

BT::NodeStatus LlmDecision::start(NebulaCtx& ctx, const Done& done) {
  const std::vector<std::string> branches =
      splitBranches(ctx.in<std::string>("branches"));
  if (branches.empty()) {
    ctx.fail("LLM_NO_BRANCHES", "branches port is empty");
    return BT::NodeStatus::FAILURE;
  }

  const nlohmann::json request = {
    {"context", ctx.in<std::string>("prompt")},
    {"branches", branches},
  };

  // 客户端在任意线程调 done;校验只依赖捕获副本(allowed/guardedDone/
  // settled),零 ctx 访问——线程安全,节点销毁也无悬挂
  const std::set<std::string> allowed(branches.begin(), branches.end());
  const int timeoutMs = ctx.in<int>("timeout_ms", 30000);
  auto settled = std::make_shared<std::atomic<bool>>(false);  // done 已被调用
  Done guardedDone = done;
  if (timeoutMs > 0) {
    guardedDone = [settled, done](BT::NodeStatus status, nlohmann::json result) {
      if (!settled->exchange(true)) {
        done(status, std::move(result));
      }
    };
    // 节点级超时看门狗:settled 置位后自然退出;guardedDone 持有的
    // shared_ptr 保全 engine/core,析构后触发仅产生丢弃告警
    std::thread([settled, done, timeoutMs] {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
      if (!settled->exchange(true)) {
        done(BT::NodeStatus::FAILURE,
             nlohmann::json{{"__fail_code", "LLM_TIMEOUT"},
                            {"__fail_message",
                             "LLM client did not respond within " +
                                 std::to_string(timeoutMs) + " ms"}});
      }
    }).detach();
  }
  client_(request, [allowed, guardedDone](BT::NodeStatus status,
                                          nlohmann::json result) {
    if (status != BT::NodeStatus::SUCCESS) {
      guardedDone(status, std::move(result));  // 客户端失败码原样透传
      return;
    }
    const std::string branch =
        result.is_object() ? result.value("branch", "") : "";
    if (branch.empty() || !allowed.count(branch)) {
      // 保留键约定:桥在调度线程内代记结构化失败
      guardedDone(BT::NodeStatus::FAILURE,
                  nlohmann::json{{"__fail_code", "LLM_INVALID_BRANCH"},
                                 {"__fail_message",
                                  "branch '" + branch +
                                      "' not in allowed set (hallucination?)"},
                                 {"branch", branch}});
      return;
    }
    guardedDone(BT::NodeStatus::SUCCESS, std::move(result));
  });
  return BT::NodeStatus::RUNNING;
}

} // namespace nebulabt
