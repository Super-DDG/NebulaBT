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

// 每次执行计数,始终失败
class Failer : public NebulaAction {
public:
  std::atomic<int> calls{0};
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override {
    calls.fetch_add(1);
    return NodeStatus::FAILURE;
  }
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

// 写/读黑板键的测试节点
class Writer : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::OutputPort<std::string>("value") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.out("value", std::string("hello"));
    return BT::NodeStatus::SUCCESS;
  }
};

class Reader : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("value") };
  }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    return BT::NodeStatus::SUCCESS;
  }
};

class ReadInt : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<int>("n") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    return ctx.in<int>("n", -1) == 7 ? BT::NodeStatus::SUCCESS
                                     : BT::NodeStatus::FAILURE;
  }
};

static void test_schema_submission_validation() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());
  const char* xml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)";

  nlohmann::json schema = {
    { "slots", {
        { "must_have", { { "type", "string" }, { "required", true } } },
        { "opt_int",   { { "type", "int" },    { "required", false } } },
    } }
  };

  TaskSpec spec;
  spec.schema = schema;

  // 缺 required 字段
  spec.slots = { { "opt_int", 5 } };
  CHECK(engine.submitFromText(xml, spec).empty());

  // 未知字段(拼写错误在加载期暴露)
  spec.slots = { { "must_have", "x" }, { "typo_key", "y" } };
  CHECK(engine.submitFromText(xml, spec).empty());

  // 类型不符
  spec.slots = { { "must_have", "x" }, { "opt_int", "not_int" } };
  CHECK(engine.submitFromText(xml, spec).empty());

  // 合法提交通过
  spec.slots = { { "must_have", "x" }, { "opt_int", 7 } };
  auto task = engine.submitFromText(xml, spec);
  CHECK(!task.empty());
  CHECK(task.wait() == TaskState::Succeeded);

  // 无 schema:完全向后兼容(未知字段不报错)
  TaskSpec legacy;
  legacy.slots = { { "anything", 1 } };
  auto legacyTask = engine.submitFromText(xml, legacy);
  CHECK(!legacyTask.empty());
  legacyTask.wait();
}

static void test_schema_tree_key_validation() {
  Engine engine;
  engine.registerAction("Writer", std::make_shared<Writer>());
  engine.registerAction("Reader", std::make_shared<Reader>());

  // 合法:被读的键由树内 Output 写出
  auto ok = engine.validate(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <Writer value="{k}"/>
        <Reader value="{k}"/>
      </Sequence>
    </BehaviorTree></root>)");
  CHECK(ok.empty());

  // 非法:读了既未声明也未写出的键
  auto bad = engine.validate(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Reader value="{never_written}"/>
    </BehaviorTree></root>)");
  CHECK(bad.size() == 1);
  CHECK(bad[0].find("never_written") != std::string::npos);
}

static void test_schema_typed_injection() {
  Engine engine;
  engine.registerAction("ReadInt", std::make_shared<ReadInt>());

  nlohmann::json schema = {
    { "slots", { { "n", { { "type", "int" }, { "required", true } } } } }
  };
  TaskSpec spec;
  spec.schema = schema;
  spec.slots = { { "n", 7 } };  // schema 声明 int -> 黑板真实 int

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ReadInt n="{n}"/>
    </BehaviorTree></root>)", spec);
  CHECK(task.wait() == TaskState::Succeeded);
}

static void test_retry_budget() {
  Engine engine;
  auto failer = std::make_shared<Failer>();
  engine.registerAction("Failer", failer);

  const auto t0 = std::chrono::steady_clock::now();
  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <RetryWithDelay attempts="100" delay_ms="20" budget_ms="80">
        <Failer/>
      </RetryWithDelay>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Failed);
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();

  // 墙钟预算(80ms)先于次数(100)耗尽:远小于 100 次执行
  CHECK(failer->calls.load() >= 2);
  CHECK(failer->calls.load() <= 10);
  CHECK(elapsedMs < 2000);
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
  test_schema_submission_validation();
  std::cout << "ok  schema submission validation\n";
  test_schema_tree_key_validation();
  std::cout << "ok  schema tree key validation\n";
  test_schema_typed_injection();
  std::cout << "ok  schema typed injection\n";
  test_retry_budget();
  std::cout << "ok  retry wall-clock budget\n";
  std::cout << "ALL TESTS PASSED\n";
  return 0;
}
