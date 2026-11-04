// NebulaBT 核心冒烟测试(零外部测试框架,CMake CTest 直接跑)
// 覆盖:同步/异步节点、ForEach、结构化失败、取消(halt 传播)、定时器

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/scheduler.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace nebulabt;
using BT::NodeStatus;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// 测试动作
// ---------------------------------------------------------------------------

class Ok : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override { return NodeStatus::SUCCESS; }
};

class Boom : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.fail("E_TEST", "boom on purpose");
    return NodeStatus::FAILURE;
  }
};

// 异步:100ms 后跨线程 done
class AsyncOk : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::OutputPort<double>("value") };
  }
  NodeStatus start(NebulaCtx&, const Done& done) override {
    std::thread([done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      done(NodeStatus::SUCCESS, { { "value", 42.0 } });
    }).detach();
    return NodeStatus::RUNNING;
  }
};

// 永不完成,可取消;halt 被调用时置标志
class Forever : public NebulaAction {
public:
  std::atomic<bool> halted{false};
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override { return NodeStatus::RUNNING; }
  void halt(NebulaCtx&) override { halted.store(true); }
};

// 记录被遍历到的元素
class Collect : public NebulaAction {
public:
  std::vector<std::string> seen;
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("item") };
  }
  NodeStatus start(NebulaCtx& ctx, const Done&) override {
    seen.push_back(ctx.in<std::string>("item"));
    return NodeStatus::SUCCESS;
  }
};

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

static void test_sync_flow_and_explain() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());
  engine.registerAction("Boom", std::make_shared<Boom>());

  auto ok = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence><Ok/><Ok/></Sequence>
    </BehaviorTree></root>)");
  CHECK(ok.wait() == TaskState::Succeeded);

  auto boom = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence><Ok/><Boom/></Sequence>
    </BehaviorTree></root>)");
  CHECK(boom.wait() == TaskState::Failed);

  // 结构化失败:code / node / message 齐全
  auto failures = boom.failures();
  CHECK(failures.size() == 1);
  CHECK(failures[0].code == "E_TEST");
  CHECK(failures[0].message == "boom on purpose");
  CHECK(!failures[0].node.empty());

  // explain:树形结构 + 失败记录
  auto report = boom.explain();
  CHECK(report["state"] == "FAILED");
  CHECK(report["tree"]["type"] == "Sequence");
  CHECK(report["failures"].size() == 1);
  std::cout << "  [explain] " << report.dump() << "\n";
}

static void test_async_wake_from_other_thread() {
  Engine engine;
  engine.registerAction("AsyncOk", std::make_shared<AsyncOk>());

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <AsyncOk value="{v}"/>
        <AsyncOk value="{v2}"/>
      </Sequence>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Succeeded);
}

static void test_for_each() {
  Engine engine;
  auto collect = std::make_shared<Collect>();
  engine.registerAction("Collect", collect);
  engine.registerAction("Ok", std::make_shared<Ok>());

  TaskSpec spec;
  spec.slots["items"] = { "a", "b", "c" };

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ForEach array="{items}" element="{item}">
        <Collect item="{item}"/>
      </ForEach>
    </BehaviorTree></root>)", spec);
  CHECK(task.wait() == TaskState::Succeeded);

  std::vector<std::string> expected{ "a", "b", "c" };
  CHECK(collect->seen == expected);

  // continue_on_failure:失败元素被跳过,整体仍成功
  auto boom = std::make_shared<Boom>();
  engine.registerAction("Boom", boom);
  TaskSpec spec2;
  spec2.slots["items"] = { "x", "y" };
  auto task2 = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ForEach array="{items}" element="{item}">
        <Boom/>
      </ForEach>
    </BehaviorTree></root>)", spec2);
  CHECK(task2.wait() == TaskState::Failed);

  auto task3 = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ForEach array="{items}" element="{item}" continue_on_failure="true">
        <Boom/>
      </ForEach>
    </BehaviorTree></root>)", spec2);
  CHECK(task3.wait() == TaskState::Succeeded);

  // 空数组:直接成功
  TaskSpec spec4;
  spec4.slots["items"] = std::vector<std::string>{};
  auto task4 = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ForEach array="{items}" element="{item}"><Ok/></ForEach>
    </BehaviorTree></root>)", spec4);
  CHECK(task4.wait() == TaskState::Succeeded);
}

static void test_cancel_halts_action() {
  Engine engine;
  auto forever = std::make_shared<Forever>();
  engine.registerAction("Forever", forever);

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Forever/>
    </BehaviorTree></root>)");

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(task.cancel());
  CHECK(task.wait() == TaskState::Canceled);
  CHECK(forever->halted.load());
}

static void test_scheduler_timer() {
  Scheduler sched;
  sched.start();

  std::atomic<bool> fired{false};
  sched.callLater(std::chrono::milliseconds(30), [&] { fired.store(true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK(fired.load());
  sched.stop();
}

static void test_on_finished_callback() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());

  std::atomic<bool> got{false};
  engine.onFinished([&](TaskHandle h) {
    CHECK(h.state() == TaskState::Succeeded);
    got.store(true);
  });

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
  task.wait();
  // 回调在调度线程执行,稍等其完成
  for (int i = 0; i < 50 && !got.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(got.load());
}

static void test_timeout_policy() {
  Engine engine;
  auto forever = std::make_shared<Forever>();
  engine.registerAction("Forever", forever);

  TaskSpec spec;
  spec.timeout_ms = 80;
  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Forever/>
    </BehaviorTree></root>)", spec);

  CHECK(task.wait() == TaskState::Failed);
  CHECK(forever->halted.load());          // 超时触发 halt 传播
  auto failures = task.failures();
  CHECK(failures.size() == 1);
  CHECK(failures[0].code == "TASK_TIMEOUT");
}

static void test_auto_failure_capture() {
  Engine engine;
  // 静默失败:返回 FAILURE 但不调 ctx.fail —— 框架兜底记录
  class SilentFail : public NebulaAction {
  public:
    BT::PortsList providedPorts() const override { return {}; }
    BT::NodeStatus start(NebulaCtx&, const Done&) override {
      return BT::NodeStatus::FAILURE;
    }
  };
  engine.registerAction("SilentFail", std::make_shared<SilentFail>());

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <SilentFail/>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Failed);

  auto failures = task.failures();
  CHECK(failures.size() == 1);
  CHECK(failures[0].code == "NODE_FAILURE");
  CHECK(failures[0].node == "SilentFail");
}

int main() {
  test_sync_flow_and_explain();
  std::cout << "ok  sync flow + explain\n";
  test_async_wake_from_other_thread();
  std::cout << "ok  async wake from other thread\n";
  test_for_each();
  std::cout << "ok  for each\n";
  test_cancel_halts_action();
  std::cout << "ok  cancel halts action\n";
  test_scheduler_timer();
  std::cout << "ok  scheduler timer\n";
  test_on_finished_callback();
  std::cout << "ok  on finished callback\n";
  test_timeout_policy();
  std::cout << "ok  timeout policy\n";
  test_auto_failure_capture();
  std::cout << "ok  auto failure capture\n";
  std::cout << "ALL TESTS PASSED\n";
  return 0;
}
