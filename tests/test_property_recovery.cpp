// ============================================================================
// 恢复性质测试 harness(M6):随机推进 × 随机崩溃 × 恢复重跑 × N 轮,
// 每轮断言核心安全不变量——
//
//   "检查点一旦在 journal 中记录完成,后续任何一次恢复重跑都不得
//    再执行该段的副作用;未完成段的副作用每轮至多执行一次"
//
// 把 kill9 的单点验证泛化为性质测试:崩溃点由虚拟时钟随机推进 +
// 随机 done 完成时机 + 随机放弃(引擎析构 = 崩溃)组合出上千种交错。
// journal 走 JournalFileWriter 帧格式(每事件 fsync),恢复侧
// readJournalFile 读回——与生产持久化路径完全一致。
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/journal_file.hpp>
#include <nebulabt/sdk/action.hpp>
#include <nebulabt/testkit/inject.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace nebulabt;
using BT::NodeStatus;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

namespace {

// 副作用账本:跨恢复轮次累计(原子:动作在调度线程执行,断言在主线程)
struct Ledger {
  std::atomic<int> a{0};
  std::atomic<int> b{0};
};

// 异步副作用动作:start 即执行副作用,done 交给测试驱动器择机完成
class AsyncStep : public NebulaAction {
public:
  AsyncStep(std::atomic<int>* effect, std::vector<Done>* pending)
    : effect_(effect), pending_(pending) {}

  BT::PortsList providedPorts() const override { return {}; }

  NodeStatus start(NebulaCtx&, const Done& done) override {
    effect_->fetch_add(1);
    pending_->push_back(done);
    return NodeStatus::RUNNING;
  }

private:
  std::atomic<int>* effect_;
  std::vector<Done>* pending_;
};

class AlwaysOk : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override {
    return NodeStatus::SUCCESS;
  }
};

static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="t">
  <BehaviorTree ID="t">
    <Sequence>
      <Checkpoint id="cp1"><StepA/></Checkpoint>
      <Checkpoint id="cp2"><StepB/></Checkpoint>
      <Tail/>
    </Sequence>
  </BehaviorTree>
</root>)";

// journal 中已记录完成的检查点 id 集合
std::vector<std::string> completedCheckpoints(const nlohmann::json& journal) {
  std::vector<std::string> ids;
  if (!journal.is_array()) {
    return ids;
  }
  for (const auto& e : journal) {
    if (e.is_object() && e.value("event", "") == "checkpoint" &&
        e.contains("data") && e["data"].contains("id")) {
      ids.push_back(e["data"]["id"].get<std::string>());
    }
  }
  return ids;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

int main() {
  const std::string journalPath = "/tmp/nebulabt_property_journal.bin";
  const int kRounds = 200;
  std::mt19937 rng(20261105);  // 固定种子:CI 可复现

  for (int round = 0; round < kRounds; ++round) {
    std::remove(journalPath.c_str());
    Ledger ledger;
    std::vector<Done> pending;
    const int tailFailFirst = static_cast<int>(rng() % 3);
    auto tail = testkit::flaky(std::make_shared<AlwaysOk>(), tailFailFirst);

    bool succeeded = false;
    for (int run = 0; run < 8 && !succeeded; ++run) {
      // 恢复输入:上一轮为止的 journal(本轮事件随后追加)
      nlohmann::json resume = nlohmann::json::array();
      if (run > 0) {
        std::string err;
        CHECK(readJournalFile(journalPath, resume, nullptr, &err));
      }
      const auto doneBefore = completedCheckpoints(resume);
      const int aBefore = ledger.a.load();
      const int bBefore = ledger.b.load();

      {
        // journal 必须比 engine 后析构:engine 析构路径的 cancel 会触发
        // journal sink 回调
        JournalFileWriter journal;
        std::string err;
        CHECK(journal.open(journalPath, /*append=*/run > 0,
                           JournalFsync::PerEvent, &err));

        Engine engine(std::make_shared<VirtualClock>());
        engine.registerAction("StepA",
                              std::make_shared<AsyncStep>(&ledger.a, &pending));
        engine.registerAction("StepB",
                              std::make_shared<AsyncStep>(&ledger.b, &pending));
        engine.registerAction("Tail", tail);

        engine.setJournalSink([&journal](const JournalEvent& e) {
          journal.append(e);
        });

        TaskSpec spec;
        spec.resume_journal = resume;
        auto task = engine.submitFromText(kXml, spec);
        CHECK(!task.empty());

        // 随机推进 + 随机完成在途动作;崩溃点 = 引擎析构
        const int steps = static_cast<int>(rng() % 6);
        for (int s = 0; s <= steps; ++s) {
          engine.advanceTime(10 + rng() % 50);
          // 每轮保证至少完成一个在途 done(存在时):否则 rng 连续抽到
          // "不完成"会让 8 轮毫无进展而耗尽(CI DIAG round 16 实证)。
          // 单调进展使耗尽在结构上不可能;"带着未完成在途动作崩溃"
          // 的多样性由后续 rng 保留
          if (!pending.empty()) {
            auto done = pending.front();
            pending.erase(pending.begin());
            done(NodeStatus::SUCCESS, {});
          }
          while (!pending.empty() && (rng() % 2) == 0) {
            auto done = pending.front();
            pending.erase(pending.begin());
            done(NodeStatus::SUCCESS, {});
          }
          if (task.waitFor(std::chrono::milliseconds(0)) !=
              TaskState::Running) {
            break;
          }
        }
        // 结算前给调度线程一个有界排空窗口:advanceTime 已把到期定时器
        // 物化为作业,落地只需真实毫秒级时间。仍在等待未触发虚拟定时器
        // 的任务这里不会完成,照常按崩溃恢复。50ms 对作业排空远超裕量
        succeeded = task.waitFor(std::chrono::milliseconds(50)) ==
                    TaskState::Succeeded;
      }  // 引擎析构:任务未终态则取消 = 模拟崩溃

      pending.clear();  // 丢弃悬挂的 done(它们持有已析构引擎的 core)

      // 核心不变量:
      //   1) 检查点已在恢复输入中完成 => 该段副作用本轮绝不再执行(= 0)
      //   2) 未完成 => 至多执行一次(0 = 崩溃前树未推进到该段,合法)
      //   3) 最终成功的那一轮:未完成段恰好执行一次
      const int aRun = ledger.a.load() - aBefore;
      const int bRun = ledger.b.load() - bBefore;
      const bool cp1Done = contains(doneBefore, "cp1");
      const bool cp2Done = contains(doneBefore, "cp2");
      if (cp1Done) CHECK(aRun == 0);
      if (cp2Done) CHECK(bRun == 0);
      if (succeeded) {
        CHECK(aRun == (cp1Done ? 0 : 1));
        CHECK(bRun == (cp2Done ? 0 : 1));
      }
    }

    if (!succeeded) {
      // 8 轮耗尽仍未成功:诊断现场(轮次内 ledger 增量与检查点完成度)
      std::cerr << "DIAG round " << round << " exhausted: aRun=" << ledger.a
                << " bRun=" << ledger.b << "\n";
    }
    CHECK(succeeded);  // 有限轮内必然恢复成功
  }

  std::cout << "ALL PROPERTY TESTS PASSED (" << kRounds << " rounds)\n";
  return 0;
}
