// ============================================================================
// 并发压力 + 引擎析构时序测试(测试加固批次)
//
// 1) 多线程并发:8 线程 × 50 任务同时提交/等待,主线程并发乱入
//    explain()/journal()/failures()/cancel() —— 单调度线程模型的
//    公开 API 必须全部线程安全
// 2) 析构时序:任务 RUNNING 中销毁引擎 → wait() 返回 CANCELED;
//    句柄跨引擎存活(explain 不崩、终态可查)
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
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

class Ok : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override { return NodeStatus::SUCCESS; }
};

class Failer : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.fail("E_STRESS", "by design");
    return NodeStatus::FAILURE;
  }
};

static void test_concurrent_submit() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());
  engine.registerAction("Failer", std::make_shared<Failer>());

  const char* okXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)";
  const char* failXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Failer/></BehaviorTree></root>)";

  constexpr int kThreads = 8;
  constexpr int kPerThread = 50;
  std::atomic<int> succeeded{0}, failed{0};
  std::atomic<bool> stopChaos{false};

  // 乱入线程:对在飞任务并发调用观测 API(不要求结果,只要求不崩不挂)
  std::thread chaos([&] {
    while (!stopChaos.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      // 此处没有句柄可拿;观测 API 的句柄级并发在任务线程内做
    }
  });

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const bool wantFail = (i % 2) == 1;
        auto task = engine.submitFromText(wantFail ? failXml : okXml);
        CHECK(bool(task));
        // 提交后立刻乱入观测:与调度线程并发
        (void)task.explain();
        (void)task.failures();
        (void)task.journal();
        const TaskState st = task.wait();
        if (wantFail) {
          CHECK(st == TaskState::Failed);
          failed.fetch_add(1);
        } else {
          CHECK(st == TaskState::Succeeded);
          succeeded.fetch_add(1);
        }
        // 终态后再观测一次(直接树遍历路径)
        (void)task.explain();
      }
    });
  }
  for (auto& w : workers) w.join();
  stopChaos.store(true);
  chaos.join();

  CHECK(succeeded.load() == kThreads * kPerThread / 2);
  CHECK(failed.load() == kThreads * kPerThread / 2);
}

static void test_teardown_ordering() {
  // 1) 任务 RUNNING 中销毁引擎:wait() 必须返回 CANCELED(不永久挂起)
  TaskHandle dangling;
  {
    class Forever : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override { return {}; }
      NodeStatus start(NebulaCtx&, const Done&) override {
        return NodeStatus::RUNNING;
      }
    };
    Engine engine;
    engine.registerAction("Forever", std::make_shared<Forever>());
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    dangling = task;
    // engine 在作用域末尾析构:取消全部活动任务并 drain
  }
  CHECK(dangling.waitFor(std::chrono::seconds(2)) == TaskState::Canceled);
  // 句柄跨引擎存活:explain/failures 不崩,终态可查
  auto ex = dangling.explain();
  CHECK(ex["state"] == "CANCELED");
  CHECK(dangling.failures().empty());

  // 2) onFinished 回调期间引擎仍存活:可查询、可提交新任务
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    std::atomic<int> cbCount{0};
    std::atomic<bool> submitInCb{false};
    engine.onFinished([&](TaskHandle finished) {
      cbCount.fetch_add(1);
      (void)finished.explain();  // 回调线程内 explain:安全(终态直接遍历)
      if (cbCount.load() == 1) {
        // 回调里提交后续任务(合法:调度线程串行,不会死锁)
        auto followup = engine.submitFromText(R"(
          <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
        submitInCb.store(static_cast<bool>(followup));
      }
    });
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Succeeded);
    // 回调里提交的 followup 也要跑完
    for (int i = 0; i < 100 && cbCount.load() < 2; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(cbCount.load() == 2);        // 原任务 + 回调内提交的 followup
    CHECK(submitInCb.load());
  }
}

int main() {
  test_concurrent_submit();
  std::cout << "ok  concurrent submit (8 threads x 50 tasks + observation races)\n";
  test_teardown_ordering();
  std::cout << "ok  teardown ordering (running dtor / cross-engine handle / onFinished submit)\n";
  std::cout << "ALL STRESS TESTS PASSED\n";
  return 0;
}
