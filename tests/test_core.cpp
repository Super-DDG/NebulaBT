// NebulaBT 核心冒烟测试(零外部测试框架,CMake CTest 直接跑)
// 覆盖:同步/异步节点、ForEach、结构化失败、取消(halt 传播)、定时器

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/journal_file.hpp>
#include <nebulabt/core/scheduler.hpp>
#include <nebulabt/nodes/llm_decision.hpp>
#include <nebulabt/sdk/action.hpp>
#include <nebulabt/testkit/mock.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
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

#define CHECK_NOTHROW(stmt)                                                    \
  do {                                                                         \
    try {                                                                      \
      stmt;                                                                    \
    } catch (const std::exception& e) {                                        \
      std::cerr << "CHECK_NOTHROW failed: " << e.what() << " (line "          \
                << __LINE__ << ")\n";                                          \
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
  if (failures.size() != 1) {
    // CI gcc-12 曾出现记录缺失;打印现场帮助定位(记录路径 or 整体为空)
    std::cerr << "DIAG failures.size=" << failures.size()
              << " state=" << toString(boom.state()) << " json="
              << nlohmann::json(failures).dump() << "\n";
  }
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

static void test_while_loop() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());
  engine.registerAction("Boom", std::make_shared<Boom>());

  // condition:前 3 次激活返回 SUCCESS,之后 FAILURE(模拟"还有活干?")
  auto condCount = std::make_shared<std::atomic<int>>(0);
  class Cond : public NebulaAction {
  public:
    std::shared_ptr<std::atomic<int>> n;
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override {
      return n->fetch_add(1) < 3 ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
    }
  };
  auto cond = std::make_shared<Cond>();
  cond->n = condCount;
  engine.registerAction("Cond", cond);

  // body:计数
  auto bodyCount = std::make_shared<std::atomic<int>>(0);
  class Body : public NebulaAction {
  public:
    std::shared_ptr<std::atomic<int>> n;
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override {
      n->fetch_add(1);
      return NodeStatus::SUCCESS;
    }
  };
  auto body = std::make_shared<Body>();
  body->n = bodyCount;
  engine.registerAction("Body", body);

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <WhileLoop><Cond/><Body/></WhileLoop>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Succeeded);   // condition FAILURE = 正常退出
  CHECK(bodyCount->load() == 3);                // body 恰好执行 3 次

  // body FAILURE -> 整体 FAILED(重置 condition 计数,先让它 SUCCESS 一次)
  condCount->store(0);
  auto task2 = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <WhileLoop><Cond/><Boom/></WhileLoop>
    </BehaviorTree></root>)");
  CHECK(task2.wait() == TaskState::Failed);

  // max_iterations 防失控:条件永真 + body 永成功,默认 1000 次后 FAILURE(而非挂死)
  class Always : public NebulaAction {
  public:
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override { return NodeStatus::SUCCESS; }
  };
  engine.registerAction("Always", std::make_shared<Always>());
  const auto t0 = std::chrono::steady_clock::now();
  auto task3 = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <WhileLoop><Always/><Always/></WhileLoop>
    </BehaviorTree></root>)");
  CHECK(task3.wait() == TaskState::Failed);     // 而不是永远 RUNNING
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
  CHECK(elapsedMs < 2000);
}

static void test_switch_n() {
  Engine engine;
  // 记录哪个分支执行了
  auto hit = std::make_shared<std::string>();
  class Branch : public NebulaAction {
  public:
    std::shared_ptr<std::string> hit;
    std::string tag;
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override {
      *hit = tag;
      return NodeStatus::SUCCESS;
    }
  };
  auto mk = [&](const std::string& tag) {
    auto n = std::make_shared<Branch>();
    n->hit = hit;
    n->tag = tag;
    engine.registerAction(tag, n);
    return n;
  };
  mk("BranchA");
  mk("BranchB");
  mk("DefaultBranch");

  const char* xml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <SwitchN variable="{mode}" case_1="a" case_2="b">
        <BranchA/>
        <BranchB/>
        <DefaultBranch/>
      </SwitchN>
    </BehaviorTree></root>)";

  TaskSpec spec;
  spec.slots["mode"] = "b";
  CHECK(engine.submitFromText(xml, spec).wait() == TaskState::Succeeded);
  CHECK(*hit == "BranchB");

  spec.slots["mode"] = "zz";  // 未命中 -> default
  CHECK(engine.submitFromText(xml, spec).wait() == TaskState::Succeeded);
  CHECK(*hit == "DefaultBranch");
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

// M2:journal + checkpoint 测试节点
class SideEffect : public NebulaAction {
public:
  std::atomic<int> calls{0};
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override {
    calls.fetch_add(1);
    return NodeStatus::SUCCESS;
  }
};

static void test_journal_events() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());

  std::atomic<int> sink_count{0};
  engine.setJournalSink([&](const JournalEvent&) { sink_count.fetch_add(1); });

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Succeeded);

  auto j = task.journal();
  CHECK(j.size() == 3);  // submitted / started / finished
  CHECK(j[0].event == "submitted");
  CHECK(j[1].event == "started");
  CHECK(j[2].event == "finished");
  CHECK(j[2].data["state"] == "SUCCEEDED");
  CHECK(sink_count.load() == 3);  // sink 与内存 journal 同步收到
}

static void test_checkpoint_resume() {
  // 树:Writer(写黑板) -> Checkpoint -> SideEffect(副作用) -> Gate(首轮失败)
  Engine engine;
  auto side = std::make_shared<SideEffect>();
  engine.registerAction("SideEffect", side);
  engine.registerAction("Writer", std::make_shared<Writer>());
  engine.registerAction("Ok", std::make_shared<Ok>());

  class Gate : public NebulaAction {
  public:
    std::atomic<bool> open{false};
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx& ctx, const Done&) override {
      if (!open.load()) {
        ctx.fail("GATE_CLOSED", "gate closed on first run");
        return NodeStatus::FAILURE;
      }
      return NodeStatus::SUCCESS;
    }
  };
  auto gate = std::make_shared<Gate>();
  engine.registerAction("Gate", gate);

  static const char* kXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <Writer value="{k}"/>
        <Checkpoint id="cp1">
          <SideEffect/>
        </Checkpoint>
        <Gate/>
      </Sequence>
    </BehaviorTree></root>)";

  // 第一轮:Gate 关闭 -> FAILED;副作用已执行 1 次,检查点已记录
  auto task1 = engine.submitFromText(kXml);
  CHECK(task1.wait() == TaskState::Failed);
  CHECK(side->calls.load() == 1);
  bool hasCp = false;
  for (const auto& e : task1.journal()) {
    if (e.event == "checkpoint" && e.data["id"] == "cp1") hasCp = true;
  }
  CHECK(hasCp);

  // 第二轮:带 resume_journal 提交,并打开 Gate
  // 检查点命中 -> Writer 跳过(黑板恢复)、SideEffect 跳过(副作用不重复!)
  gate->open.store(true);
  TaskSpec spec;
  spec.resume_journal = nlohmann::json::array();
  for (const auto& e : task1.journal()) {
    spec.resume_journal.push_back(e);
  }
  auto task2 = engine.submitFromText(kXml, spec);
  CHECK(task2.wait() == TaskState::Succeeded);
  CHECK(side->calls.load() == 1);  // 副作用没有重复执行!

  bool hasRestore = false;
  for (const auto& e : task2.journal()) {
    if (e.event == "checkpoint_restored" && e.data["id"] == "cp1")
      hasRestore = true;
  }
  CHECK(hasRestore);
}

static void test_checkpoint_resume_across_engines() {
  // 模拟跨进程恢复:sink 落盘(每事件一行 JSON)-> 新 Engine 读回 -> 恢复
  static const char* kXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <Writer value="{k}"/>
        <Checkpoint id="cp1">
          <SideEffect/>
        </Checkpoint>
        <Ok/>
      </Sequence>
    </BehaviorTree></root>)";

  std::vector<std::string> persisted;
  {
    Engine engine;
    auto side = std::make_shared<SideEffect>();
    engine.registerAction("SideEffect", side);
    engine.registerAction("Writer", std::make_shared<Writer>());
    engine.registerAction("Ok", std::make_shared<Ok>());
    engine.setJournalSink([&](const JournalEvent& e) {
      persisted.push_back(nlohmann::json(e).dump());
    });

    auto task = engine.submitFromText(kXml);
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(side->calls.load() == 1);
  }  // "进程退出"

  // 新进程:读回 journal,恢复执行
  Engine engine;
  auto side = std::make_shared<SideEffect>();
  engine.registerAction("SideEffect", side);
  engine.registerAction("Writer", std::make_shared<Writer>());
  engine.registerAction("Ok", std::make_shared<Ok>());

  TaskSpec spec;
  for (const auto& line : persisted) {
    spec.resume_journal.push_back(nlohmann::json::parse(line));
  }
  auto task = engine.submitFromText(kXml, spec);
  CHECK(task.wait() == TaskState::Succeeded);
  CHECK(side->calls.load() == 0);  // 副作用从未在新进程中执行——检查点跳过
}

static void test_explain_timing() {
  Engine engine;
  engine.registerAction("AsyncOk", std::make_shared<AsyncOk>());
  engine.registerAction("Ok", std::make_shared<Ok>());

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <AsyncOk value="{v}"/>
        <Ok/>
      </Sequence>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Succeeded);

  auto report = task.explain();
  const auto& tree = report["tree"];
  CHECK(tree["children"][0]["type"] == "AsyncOk");
  // 异步节点:1 次 attempt,RUNNING 时长 ≈ 50ms(允许调度误差)
  const int attempts = tree["children"][0]["attempts"].get<int>();
  const double runningMs = tree["children"][0]["running_ms"].get<double>();
  CHECK(attempts == 1);
  CHECK(runningMs >= 40.0);
  CHECK(tree["children"][1]["attempts"].get<int>() == 1);  // 同步 Ok
}

// ---------------------------------------------------------------------------
// M3:树包 / 灰度 / 测试替身
// ---------------------------------------------------------------------------

#include <sys/stat.h>

static void writePack(const std::string& dir, const std::string& name,
                      const std::string& version,
                      const std::string& treeBody) {
  // mkdir 不递归:先建父目录,再建子目录
  mkdir("/tmp/nebulabt_pack_test", 0755);
  mkdir(dir.c_str(), 0755);
  const std::string treesDir = dir + "/trees";
  mkdir(treesDir.c_str(), 0755);
  std::ofstream(dir + "/pack.json")
      << "{\"name\":\"" << name << "\",\"version\":\"" << version
      << "\",\"task_types\":{\"M3_TASK\":{"
      << "\"tree\":\"m3_tree\","
      << "\"schema\":{\"slots\":{\"n\":{\"type\":\"int\",\"required\":true}}},"
      << "\"failure_codes\":{\"E_DECLARED\":\"declared in pack manifest\"}}}}";
  std::ofstream(treesDir + "/m3_tree.xml") << treeBody;
}

// 按黑板 n 的奇偶决定成败(用于区分两个版本的行为)
class ParityAction : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<int>("n"), BT::OutputPort<std::string>("which") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const int n = ctx.in<int>("n", 0);
    ctx.out("which", (n % 2 == 0) ? "even" : "odd");
    return BT::NodeStatus::SUCCESS;
  }
};

static void test_pack_and_canary() {
  const std::string base = "/tmp/nebulabt_pack_test";
  // 稳定版 1.0.0:读 n,输出 even 分支内容;金丝雀 2.0.0:odd 分支内容
  writePack(base + "/v1", "m3pack", "1.0.0", R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ParityAction n="{n}" which="{which}"/>
    </BehaviorTree></root>)");
  writePack(base + "/v2", "m3pack", "2.0.0", R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ParityAction n="{n}" which="{which}"/>
    </BehaviorTree></root>)");

  Engine engine;
  engine.registerAction("ParityAction", std::make_shared<ParityAction>());
  CHECK_NOTHROW(engine.loadPack(base + "/v1"));
  CHECK_NOTHROW(engine.loadPack(base + "/v2"));

  // 显式钉住 1.0.0
  TaskSpec spec;
  spec.task_type = "M3_TASK";
  spec.pack_version = "1.0.0";
  spec.slots["n"] = 4;
  auto t1 = engine.submitTask(spec);
  CHECK(!t1.empty());
  CHECK(t1.wait() == TaskState::Succeeded);
  CHECK(t1.explain()["pack_version"] == "1.0.0");

  // 灰度 100% -> 全部路由到 2.0.0
  engine.setCanary("m3pack", "2.0.0", 100);
  TaskSpec spec2;
  spec2.task_type = "M3_TASK";
  spec2.slots["n"] = 4;
  auto t2 = engine.submitTask(spec2);
  CHECK(!t2.empty());
  CHECK(t2.wait() == TaskState::Succeeded);
  CHECK(t2.explain()["pack_version"] == "2.0.0");

  // 灰度 0% -> 回到最高版本(2.0.0 仍是最高版本,显式对比:钉 1.0.0)
  engine.setCanary("m3pack", "2.0.0", 0);
  TaskSpec spec3;
  spec3.task_type = "M3_TASK";
  spec3.slots["n"] = 4;
  spec3.pack_version = "1.0.0";
  CHECK(engine.submitTask(spec3).explain()["pack_version"] == "1.0.0");

  // M8 回归:pack 清单声明的失败码必须在 move 前登记(moved-from 为空的
  // 旧实现使 failure_codes 从未注册)。声明码 → undeclared_code=false,
  // 未声明码 → true
  {
    writePack(base + "/fpack", "fpack", "1.0.0", R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Fallback>
          <FailAlways/>
          <FailUndeclared/>
        </Fallback>
      </BehaviorTree></root>)");

    class CodedFail : public NebulaAction {
    public:
      explicit CodedFail(std::string code) : code_(std::move(code)) {}
      BT::PortsList providedPorts() const override { return {}; }
      BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
        ctx.fail(code_, "by design");
        return BT::NodeStatus::FAILURE;
      }

    private:
      std::string code_;
    };

    Engine engine2;
    engine2.registerAction("FailAlways", std::make_shared<CodedFail>("E_DECLARED"));
    engine2.registerAction("FailUndeclared", std::make_shared<CodedFail>("E_OTHER"));
    CHECK_NOTHROW(engine2.loadPack(base + "/fpack"));

    TaskSpec s;
    s.task_type = "M3_TASK";
    s.slots["n"] = 2;
    auto t = engine2.submitTask(s);
    CHECK(t.wait() == TaskState::Failed);
    const auto failures = t.failures();
    CHECK(failures.size() == 2);
    CHECK(failures[0].code == "E_DECLARED");
    CHECK(failures[0].undeclared_code == false);  // pack 声明的码已登记
    CHECK(failures[1].code == "E_OTHER");
    CHECK(failures[1].undeclared_code == true);   // 未登记码被标记
    // loadedTaskTypes:move 后清单仍正确
    bool found = false;
    for (const auto& ti : engine2.loadedTaskTypes()) {
      if (ti.task_type == "M3_TASK" && ti.pack == "fpack") {
        found = true;
      }
    }
    CHECK(found);
  }

  // 重复加载同版本 -> 抛异常
  bool threw = false;
  try {
    engine.loadPack(base + "/v1");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

static void test_scripted_mock() {
  using nebulabt::testkit::ScriptedAction;
  Engine engine;
  auto nav = std::make_shared<ScriptedAction>(BT::PortsList{
      BT::InputPort<std::string>("point_id")});
  nav->thenFail("NAV_TIMEOUT", "no route")
      .then(BT::NodeStatus::SUCCESS, {{"arrived", true}});
  engine.registerAction("Nav", nav);

  static const char* kXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <RetryWithDelay attempts="3" delay_ms="10">
        <Nav point_id="{loc}"/>
      </RetryWithDelay>
    </BehaviorTree></root>)";
  TaskSpec spec;
  spec.slots["loc"] = "kitchen";
  auto task = engine.submitFromText(kXml, spec);
  CHECK(task.wait() == TaskState::Succeeded);  // 第 1 次失败,第 2 次成功

  const auto calls = nav->calls();
  CHECK(calls.size() == 2);
  CHECK(calls[0]["point_id"] == "kitchen");   // 输入快照被录制
  CHECK(calls[1]["point_id"] == "kitchen");
}

static void test_template_and_requires() {
  // pack:模板 "work" 声明树+schema;任务经 template 引用,slot_defaults 给默认值
  const std::string dir = "/tmp/nebulabt_tpl_test";
  mkdir("/tmp/nebulabt_tpl_test", 0755);
  mkdir(dir.c_str(), 0755);
  mkdir((dir + "/trees").c_str(), 0755);
  std::ofstream(dir + "/pack.json") << R"({
    "name": "tplpack", "version": "1.0.0",
    "templates": {
      "work": { "tree": "tpl_tree",
                "schema": {"slots": {"n": {"type": "int", "required": true}}} }
    },
    "task_types": {
      "TPL_TASK": { "template": "work", "slot_defaults": {"n": 7} }
    }
  })";
  std::ofstream(dir + "/trees/tpl_tree.xml") << R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ReadInt n="{n}"/>
    </BehaviorTree></root>)";

  Engine engine;
  engine.registerAction("ReadInt", std::make_shared<ReadInt>());
  engine.loadPack(dir);

  // 提交未给 n -> slot_defaults 注入 7 -> ReadInt 判定成功(模板+默认值贯通)
  TaskSpec spec;
  spec.task_type = "TPL_TASK";
  auto task = engine.submitTask(spec);
  CHECK(!task.empty());
  CHECK(task.wait() == TaskState::Succeeded);

  // requires:依赖未加载的 pack -> loadPack 抛异常;补齐后通过
  const std::string dir2 = "/tmp/nebulabt_req_test";
  mkdir("/tmp/nebulabt_req_test", 0755);
  mkdir(dir2.c_str(), 0755);
  mkdir((dir2 + "/trees").c_str(), 0755);
  std::ofstream(dir2 + "/pack.json") << R"({
    "name": "reqpack", "version": "1.0.0",
    "requires": [{"pack": "tplpack", "min_version": "1.0.0"}],
    "task_types": {
      "REQ_TASK": { "template": "work" }
    },
    "templates": {
      "work": { "tree": "tpl_tree",
                "schema": {"slots": {"n": {"type": "int", "required": true}}} }
    }
  })";
  std::ofstream(dir2 + "/trees/tpl_tree.xml") << R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <ReadInt n="{n}"/>
    </BehaviorTree></root>)";

  Engine fresh;  // tplpack 未加载
  fresh.registerAction("ReadInt", std::make_shared<ReadInt>());
  bool threw = false;
  try {
    fresh.loadPack(dir2);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);  // 依赖未满足
  fresh.loadPack(dir);   // 先加载依赖
  fresh.loadPack(dir2);  // 再加载依赖方:通过
}

// 回归:循环体内的 Checkpoint 不能被本 run 刚记录的检查点误跳过
// (曾有 bug:record 与 resume 历史共用一个 map,第二次迭代被静默跳过)
static void test_checkpoint_in_loop() {
  Engine engine;
  auto side = std::make_shared<SideEffect>();
  engine.registerAction("SideEffect", side);
  engine.registerAction("Ok", std::make_shared<Ok>());

  auto condCount = std::make_shared<std::atomic<int>>(0);
  class Cond : public NebulaAction {
  public:
    std::shared_ptr<std::atomic<int>> n;
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override {
      return n->fetch_add(1) < 2 ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
    }
  };
  auto cond = std::make_shared<Cond>();
  cond->n = condCount;
  engine.registerAction("Cond", cond);

  auto task = engine.submitFromText(R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <WhileLoop>
        <Cond/>
        <Sequence>
          <Checkpoint id="loop_cp">
            <SideEffect/>
          </Checkpoint>
          <Ok/>
        </Sequence>
      </WhileLoop>
    </BehaviorTree></root>)");
  CHECK(task.wait() == TaskState::Succeeded);
  CHECK(side->calls.load() == 2);  // 两次迭代都执行了,而非被误跳过只剩 1 次
}

// ---- M4.1:journal 文件掉电安全(帧格式 + CRC + 撕裂容错)----
static void test_journal_file() {
  const std::string path = "/tmp/nebulabt_test_journal.bin";
  std::remove(path.c_str());

  JournalEvent a;
  a.task_id = "t1"; a.task_type = "demo";
  a.event = "submitted"; a.data = nlohmann::json{{"k", 1}};
  JournalEvent b;
  b.task_id = "t1"; b.event = "checkpoint";
  b.data = nlohmann::json{{"id", "cp1"}, {"blackboard", nlohmann::json::object()}};

  {
    JournalFileWriter w;
    CHECK(w.open(path, false, JournalFsync::PerEvent));
    CHECK(w.append(a));
    CHECK(w.append(b));
  }
  nlohmann::json arr;
  JournalFileReadStats st;
  CHECK(readJournalFile(path, arr, &st));
  CHECK(arr.size() == 2);
  CHECK(!st.torn_tail);
  CHECK(arr[0]["event"] == "submitted");
  CHECK(arr[1]["data"]["id"] == "cp1");

  // 掉电模拟 1:帧头被截断(半截帧)→ 完好前缀保留 + torn_tail 标记
  {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    const char torn[] = {0x40, 0x00, 0x00, 0x00, 0x11};
    f.write(torn, sizeof(torn));
  }
  CHECK(readJournalFile(path, arr, &st));
  CHECK(arr.size() == 2);
  CHECK(st.torn_tail);

  // 掉电模拟 2:帧头完整但 payload 写坏(CRC 不符)→ 同样容错
  std::remove(path.c_str());
  {
    JournalFileWriter w;
    CHECK(w.open(path, false));
    CHECK(w.append(a));
  }
  {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    const std::string payload = "{\"x\":1}";
    std::vector<uint8_t> frame{0x07, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};
    frame.insert(frame.end(), payload.begin(), payload.end());
    f.write(reinterpret_cast<const char*>(frame.data()),
            static_cast<std::streamsize>(frame.size()));
  }
  CHECK(readJournalFile(path, arr, &st));
  CHECK(arr.size() == 1);
  CHECK(st.torn_tail);

  std::remove(path.c_str());
}

// ---- M4.2:资源上限 ----
static void test_limits() {
  // scheduler 队列上限:满则拒绝
  {
    Scheduler sched;
    sched.start();
    sched.setLimits(1, 0);
    std::atomic<bool> release{false};
    CHECK(sched.post([&release] {
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 占住调度线程
    CHECK(sched.post([] {}));   // 1/1:入队
    CHECK(!sched.post([] {}));  // 满了:拒绝
    release.store(true);
    sched.stop();
  }

  // 活动任务上限:超限提交返回空句柄,取消后名额释放
  {
    Engine engine;
    engine.registerAction("Forever", std::make_shared<Forever>());
    EngineLimits limits;
    limits.max_active_tasks = 2;
    engine.setLimits(limits);

    const char* xml = R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)";
    TaskSpec s1, s2, s3;
    s1.task_id = "a"; s2.task_id = "b"; s3.task_id = "c";
    auto t1 = engine.submitFromText(xml, s1);
    auto t2 = engine.submitFromText(xml, s2);
    CHECK(bool(t1) && bool(t2));
    auto t3 = engine.submitFromText(xml, s3);
    CHECK(!t3);  // 超限拒绝
    CHECK(t1.cancel());
    CHECK(t1.waitFor(std::chrono::milliseconds(500)) == TaskState::Canceled);
    auto t4 = engine.submitFromText(xml, s3);
    CHECK(bool(t4));  // 名额已释放
    CHECK(t2.cancel());
    CHECK(t4.cancel());
  }
}

// ---- M6.1:虚拟时钟(重试/超时/watchdog 零真实等待)----
static void test_virtual_clock() {
  // 1) RetryWithDelay:延时随 advanceTime 推进,重试耗尽即失败
  {
    auto clock = std::make_shared<VirtualClock>();
    Engine engine(clock);
    auto failer = std::make_shared<Failer>();
    engine.registerAction("Failer", failer);

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <RetryWithDelay attempts="3" delay_ms="1000" budget_ms="100000">
          <Failer/>
        </RetryWithDelay>
      </BehaviorTree></root>)");
    CHECK(task.waitFor(std::chrono::milliseconds(50)) == TaskState::Running);
    engine.advanceTime(1000);   // 第 1 次重试延时到期
    engine.advanceTime(1000);   // 第 2 次重试延时到期
    CHECK(task.waitFor(std::chrono::milliseconds(100)) == TaskState::Failed);
    CHECK(failer->calls.load() == 3);
  }

  // 2) 任务超时:到点即触发,零真实等待
  {
    auto clock = std::make_shared<VirtualClock>();
    Engine engine(clock);
    engine.registerAction("Forever", std::make_shared<Forever>());

    TaskSpec spec;
    spec.timeout_ms = 5000;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)",
      spec);
    CHECK(task.waitFor(std::chrono::milliseconds(50)) == TaskState::Running);
    engine.advanceTime(4999);
    CHECK(task.waitFor(std::chrono::milliseconds(50)) == TaskState::Running);
    engine.advanceTime(1);
    CHECK(task.waitFor(std::chrono::milliseconds(100)) == TaskState::Failed);
    auto failures = task.failures();
    CHECK(failures.size() == 1);
    CHECK(failures[0].code == "TASK_TIMEOUT");
  }

  // 3) watchdog:静默随虚拟时间累积,触发一次
  {
    auto clock = std::make_shared<VirtualClock>();
    Engine engine(clock);
    auto forever = std::make_shared<Forever>();
    engine.registerAction("Forever", forever);
    std::atomic<int> fired{0};
    engine.setWatchdog(std::chrono::milliseconds(100), true,
                       [&fired](const std::string&, uint64_t quiet) {
                         fired.fetch_add(1);
                         CHECK(quiet >= 100);
                       });
    TaskSpec spec;
    spec.watchdog_ms = 100;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)",
      spec);
    engine.advanceTime(99);
    CHECK(task.waitFor(std::chrono::milliseconds(50)) == TaskState::Running);
    engine.advanceTime(1);
    CHECK(task.waitFor(std::chrono::milliseconds(100)) == TaskState::Failed);
    CHECK(fired.load() == 1);
    CHECK(forever->halted.load());
  }

  // 4) 真实时钟引擎上 advanceTime 无效(告警,不崩溃)
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    engine.advanceTime(1000);  // 仅告警
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Succeeded);
  }
}

// ---- M7:Groot2 对接 / 运行中 explain / 失败富化 ----
static void test_m7_observability() {
  // 1) 失败自动富化:failure.inputs 携带激活时刻的输入端口快照
  {
    Engine engine;
    class Boom : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override {
        return { BT::InputPort<std::string>("sku"),
                 BT::InputPort<int>("qty") };
      }
      NodeStatus start(NebulaCtx& ctx, const Done&) override {
        ctx.fail("E_BOOM", "injected");
        return NodeStatus::FAILURE;
      }
    };
    engine.registerAction("Boom", std::make_shared<Boom>());

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Boom sku="{sku}" qty="{qty}"/>
      </BehaviorTree></root>)",
      [&] {
        TaskSpec s;
        s.slots["sku"] = "sku-9";
        s.slots["qty"] = 3;
        return s;
      }());
    CHECK(task.wait() == TaskState::Failed);
    auto failures = task.failures();
    CHECK(failures.size() == 1);
    CHECK(failures[0].inputs["sku"] == "sku-9");
    CHECK(failures[0].inputs["qty"] == 3);
  }

  // 2) 运行中 explain:任务 RUNNING 时安全获取树快照
  {
    Engine engine;
    engine.registerAction("Forever", std::make_shared<Forever>());
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const nlohmann::json j = task.explain();  // RUNNING 中调用(M7 前不安全)
    CHECK(j["state"] == "RUNNING");
    CHECK(j.contains("tree"));
    CHECK(j["tree"]["status"] == "RUNNING");
    CHECK(task.cancel());
  }

  // 3) Groot2 publisher:创建、运行、析构全程无异常
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    TaskSpec spec;
    spec.groot2_port = 27183;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)",
      spec);
    CHECK(task.wait() == TaskState::Succeeded);
  }
}

// ---- 代码审查修复批次:A1-A4 + B5/B6/B7 ----
static void test_review_fixes() {
  // A2:logger 重入 Engine API 不死锁(旧代码 logf 持锁回调)
  {
    Engine engine;
    std::atomic<int> logCalls{0};
    engine.setLogger([&engine, &logCalls](LogLevel, const std::string&) {
      logCalls.fetch_add(1);
      engine.setLogger([](LogLevel, const std::string&) {});
    });
    engine.registerAction("Boom", std::make_shared<Boom>());
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Boom/></BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    CHECK(logCalls.load() >= 1);  // 没卡死且确实经过了 logger
  }

  // A1+A4+B6:调度线程被占住时 ——
  //   explain 超时返回后迟到 job 无 UB(A1)、队列满 explain 立即降级(A4)、
  //   首次 tick 投递被拒的提交不留僵尸任务(B6)
  {
    class Block2s : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override { return {}; }
      NodeStatus start(NebulaCtx&, const Done&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2300));
        return NodeStatus::SUCCESS;
      }
    };
    Engine engine;
    engine.registerAction("Block2s", std::make_shared<Block2s>());
    engine.registerAction("Ok", std::make_shared<Ok>());
    EngineLimits lim;
    lim.max_pending_jobs = 1;
    engine.setLimits(lim);
    engine.setLogger([](LogLevel, const std::string&) {});  // 静音拒绝告警

    auto tA = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Block2s/></BehaviorTree></root>)");
    CHECK(bool(tA));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // A 执行中,队列空

    // A1:A 占住调度线程,快照 job 无法执行 → 2s 超时返回;
    // 旧代码此时 job 悬垂,稍后执行即 UB
    auto t0 = std::chrono::steady_clock::now();
    auto j1 = tA.explain();
    const auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(d1 >= 1500);                 // 走的是超时路径
    CHECK(j1.contains("tree_snapshot"));  // 降级而非崩溃
    CHECK(!j1.contains("tree"));

    // B6:队列被快照 job 占满,新任务首次 tick 投递被拒 → 空句柄,不留僵尸
    auto tB = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)");
    CHECK(!tB);

    // A4:队列满,explain 的 post 被拒 → 立即降级(旧代码白等 2s)
    t0 = std::chrono::steady_clock::now();
    auto j2 = tA.explain();
    const auto d2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(d2 < 500);
    CHECK(j2.contains("tree_snapshot"));

    CHECK(tA.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
    // 迟到的快照 job 已在共享状态上安全执行(A1 修复的直接验证)
    auto j3 = tA.explain();
    CHECK(j3.contains("tree"));
  }

  // A3:CRC 表并发初始化(现为编译期 constexpr,并发首调无竞态)
  {
    std::atomic<int> failed{0};
    std::vector<std::thread> ths;
    for (int t = 0; t < 4; ++t) {
      ths.emplace_back([&, t] {
        const std::string path =
            "/tmp/nebulabt_crc_race_" + std::to_string(t) + ".bin";
        std::remove(path.c_str());
        JournalFileWriter w;
        if (!w.open(path, false)) {
          failed.fetch_add(1);
          return;
        }
        for (int i = 0; i < 50; ++i) {
          JournalEvent e;
          e.task_id = "t";
          e.event = "submitted";
          e.data = nlohmann::json{{"i", i}};
          if (!w.append(e)) {
            failed.fetch_add(1);
            return;
          }
        }
        nlohmann::json arr;
        JournalFileReadStats st;
        if (!readJournalFile(path, arr, &st) || arr.size() != 50 ||
            st.torn_tail) {
          failed.fetch_add(1);
        }
        std::remove(path.c_str());
      });
    }
    for (auto& th : ths) th.join();
    CHECK(failed.load() == 0);
  }

  // B5:调度线程内调用 advanceTime(旧代码两阶段等待自死锁)
  {
    class AdvanceInside : public NebulaAction {
    public:
      explicit AdvanceInside(Engine* e) : e_(e) {}
      BT::PortsList providedPorts() const override { return {}; }
      NodeStatus start(NebulaCtx&, const Done&) override {
        e_->advanceTime(10);  // 应被拒绝而非死锁
        return NodeStatus::SUCCESS;
      }

    private:
      Engine* e_;
    };
    auto vclock = std::make_shared<VirtualClock>();
    Engine engine(vclock);
    engine.registerAction("AdvanceInside", std::make_shared<AdvanceInside>(&engine));
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><AdvanceInside/></BehaviorTree></root>)");
    CHECK(task.waitFor(std::chrono::seconds(2)) == TaskState::Succeeded);
  }

  // B7:journal 内存上限可配 + 触顶告警一次(sink 不受影响)
  {
    Engine engine;
    engine.registerAction("SideEffect", std::make_shared<SideEffect>());
    engine.registerAction("Ok", std::make_shared<Ok>());
    std::atomic<int> warns{0};
    engine.setLogger([&warns](LogLevel lv, const std::string&) {
      if (lv == LogLevel::Warn) warns.fetch_add(1);
    });
    EngineLimits lim;
    lim.max_journal_events = 3;
    engine.setLimits(lim);

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <Checkpoint id="cp"><SideEffect/></Checkpoint>
          <Ok/>
        </Sequence>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Succeeded);
    // 事件序列 submitted/started/checkpoint/finished:内存截到 3 条
    CHECK(task.journal().size() == 3);
    CHECK(warns.load() >= 1);
  }
}

// ---- M8:失败码登记表 + LLMDecision ----
static void test_m8_agent() {
  // 1) 失败码登记表:未登记的码被标记 undeclared_code
  {
    Engine engine;
    engine.declareFailureCodes({{"E_KNOWN", "a known code"}});
    engine.registerAction("Boom", std::make_shared<Boom>());

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Boom/></BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    auto failures = task.failures();
    CHECK(failures.size() == 1);
    CHECK(failures[0].code == "E_TEST");        // Boom 用的是未登记码
    CHECK(failures[0].undeclared_code == true);

    // 登记后同一动作不再被标记
    engine.declareFailureCodes({{"E_TEST", "boom on purpose"}});
    auto task2 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Boom/></BehaviorTree></root>)");
    CHECK(task2.wait() == TaskState::Failed);
    CHECK(task2.failures()[0].undeclared_code == false);
  }

  // 2) LLMDecision:合法分支 → 输出写回;幻觉分支 → LLM_INVALID_BRANCH
  {
    Engine engine;
    // 捕获决策输出
    auto observed = std::make_shared<std::string>();
    class Observe : public NebulaAction {
    public:
      std::shared_ptr<std::string> out;
      BT::PortsList providedPorts() const override {
        return { BT::InputPort<std::string>("branch") };
      }
      NodeStatus start(NebulaCtx& ctx, const Done&) override {
        *out = ctx.in<std::string>("branch");
        return NodeStatus::SUCCESS;
      }
    };
    auto obs = std::make_shared<Observe>();
    obs->out = observed;
    engine.registerAction("Observe", obs);

    // 同步 mock 客户端:第一次返回合法分支,第二次幻觉
    auto callCount = std::make_shared<std::atomic<int>>(0);
    LlmDecision::Client client = [callCount](const nlohmann::json& req,
                                             Done done) {
      const bool hallucinate = callCount->fetch_add(1) == 1;
      done(NodeStatus::SUCCESS, nlohmann::json{
        {"branch", hallucinate ? "teleport" : "patrol"},
        {"reason", "mock"},
      });
    };
    engine.registerAction("LLMDecision",
                          std::make_shared<LlmDecision>(client));

    const char* xml = R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <LLMDecision prompt="{situation}" branches="patrol,engage,retreat"
                       branch="{decision}"/>
          <Observe branch="{decision}"/>
        </Sequence>
      </BehaviorTree></root>)";

    TaskSpec s1;
    s1.slots["situation"] = "quiet sector";
    auto t1 = engine.submitFromText(xml, s1);
    CHECK(t1.wait() == TaskState::Succeeded);
    CHECK(*observed == "patrol");               // 分支写回 Output 端口

    TaskSpec s2;
    s2.slots["situation"] = "anything";
    auto t2 = engine.submitFromText(xml, s2);
    CHECK(t2.wait() == TaskState::Failed);      // 幻觉分支被拦截
    auto failures = t2.failures();
    CHECK(failures.size() == 1);
    CHECK(failures[0].code == "LLM_INVALID_BRANCH");
    CHECK(failures[0].node_type == "LLMDecision");
  }

  // 3) 异步客户端(独立线程)+ branches 为空
  {
    Engine engine;
    LlmDecision::Client asyncClient = [](const nlohmann::json&, Done done) {
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        done(NodeStatus::SUCCESS, nlohmann::json{{"branch", "engage"},
                                                 {"reason", "threat"}});
      }).detach();
    };
    engine.registerAction("LLMDecision",
                          std::make_shared<LlmDecision>(asyncClient));
    engine.registerAction("Ok", std::make_shared<Ok>());

    auto t1 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <LLMDecision prompt="x" branches="patrol,engage" branch="{d}"/>
          <Ok/>
        </Sequence>
      </BehaviorTree></root>)");
    CHECK(t1.wait() == TaskState::Succeeded);

    auto t2 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <LLMDecision prompt="x" branches="" branch="{d}"/>
      </BehaviorTree></root>)");
    CHECK(t2.wait() == TaskState::Failed);
    CHECK(t2.failures()[0].code == "LLM_NO_BRANCHES");
  }
}

// ---- M5.1:Compensable Saga 补偿 ----
static void test_compensable() {
  // 补偿动作:按执行顺序记录释放的 id
  class Release : public NebulaAction {
  public:
    std::shared_ptr<std::vector<std::string>> log;
    BT::PortsList providedPorts() const override {
      return { BT::InputPort<std::string>("id") };
    }
    NodeStatus start(NebulaCtx& ctx, const Done&) override {
      log->push_back(ctx.in<std::string>("id"));
      return NodeStatus::SUCCESS;
    }
  };

  const char* sagaXml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t">
      <Sequence>
        <Compensable id="reserve">
          <SideEffect/>
          <Release id="release_reserve"/>
        </Compensable>
        <Compensable id="charge">
          <SideEffect/>
          <Release id="release_charge"/>
        </Compensable>
        <Trailing/>
      </Sequence>
    </BehaviorTree></root>)";

  // 1) 尾随步骤失败 → 逆序自动补偿
  {
    Engine engine;
    auto side = std::make_shared<SideEffect>();
    auto released = std::make_shared<std::vector<std::string>>();
    auto rel = std::make_shared<Release>();
    rel->log = released;
    engine.registerAction("SideEffect", side);
    engine.registerAction("Release", rel);
    engine.registerAction("Trailing", std::make_shared<Failer>());

    auto task = engine.submitFromText(sagaXml);
    CHECK(task.wait() == TaskState::Failed);
    CHECK(side->calls.load() == 2);       // 两个主工作都执行过
    CHECK(released->size() == 2);
    CHECK((*released)[0] == "release_charge");  // 逆序:后完成的先补偿
    CHECK((*released)[1] == "release_reserve");

    bool armed = false, started = false, done = false;
    for (const auto& e : task.journal()) {
      if (e.event == "compensation_armed") armed = true;
      if (e.event == "compensation_started") started = true;
      if (e.event == "compensation_done") done = true;
    }
    CHECK(armed && started && done);
  }

  // 2) 全部成功 → 补偿永不执行
  {
    Engine engine;
    auto released = std::make_shared<std::vector<std::string>>();
    auto rel = std::make_shared<Release>();
    rel->log = released;
    engine.registerAction("SideEffect", std::make_shared<SideEffect>());
    engine.registerAction("Release", rel);
    engine.registerAction("Trailing", std::make_shared<Ok>());

    auto task = engine.submitFromText(sagaXml);
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(released->empty());
  }

  // 3) 超时失败也触发补偿:主工作已完成,后续长步骤被超时打断
  {
    Engine engine;
    auto side = std::make_shared<SideEffect>();
    auto released = std::make_shared<std::vector<std::string>>();
    auto rel = std::make_shared<Release>();
    rel->log = released;
    engine.registerAction("SideEffect", side);
    engine.registerAction("Forever", std::make_shared<Forever>());
    engine.registerAction("Release", rel);

    TaskSpec spec;
    spec.timeout_ms = 80;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <Compensable id="reserve">
            <SideEffect/>
            <Release id="release_reserve"/>
          </Compensable>
          <Forever/>
        </Sequence>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Failed);
    CHECK(side->calls.load() == 1);
    CHECK(released->size() == 1);   // 超时照样补偿已完成的副作用
    CHECK((*released)[0] == "release_reserve");
  }

  // 4) 取消不触发补偿(取消语义由业务在 halt() 里决定)
  {
    Engine engine;
    auto side = std::make_shared<SideEffect>();
    auto released = std::make_shared<std::vector<std::string>>();
    auto rel = std::make_shared<Release>();
    rel->log = released;
    engine.registerAction("SideEffect", side);
    engine.registerAction("Forever", std::make_shared<Forever>());
    engine.registerAction("Release", rel);

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <Compensable id="reserve">
            <SideEffect/>
            <Release id="release_reserve"/>
          </Compensable>
          <Forever/>
        </Sequence>
      </BehaviorTree></root>)");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 等主工作完成
    CHECK(side->calls.load() == 1);
    CHECK(task.cancel());
    CHECK(task.waitFor(std::chrono::milliseconds(500)) == TaskState::Canceled);
    CHECK(released->empty());
  }
}

// ---- M5.2:ReactiveGuard 运行期守护 ----
static void test_reactive_guard() {
  // 1) 守护破坏 → halt 子树 + GUARD_VIOLATED + 父层恢复分支
  {
    // 启动子树后把守护键翻转为 false(经 Output 端口写黑板)
    class FlipFalse : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override {
        return { BT::OutputPort<bool>("out") };
      }
      NodeStatus start(NebulaCtx& ctx, const Done&) override {
        ctx.out("out", false);
        return NodeStatus::SUCCESS;
      }
    };
    Engine engine;
    auto forever = std::make_shared<Forever>();
    engine.registerAction("Forever", forever);
    engine.registerAction("FlipFalse", std::make_shared<FlipFalse>());
    engine.registerAction("Ok", std::make_shared<Ok>());

    TaskSpec spec;
    spec.slots["safe"] = true;
    // schema 声明 → safe 按真实 bool 注入(FlipFalse 之后写 bool 才不冲突)
    spec.schema = nlohmann::json{
        {"slots", {{"safe", {{"type", "bool"}, {"required", true}}}}}};
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Fallback>
          <ReactiveGuard while_true="{safe}" period_ms="40">
            <Sequence>
              <FlipFalse out="{safe}"/>
              <Forever/>
            </Sequence>
          </ReactiveGuard>
          <Ok/>
        </Fallback>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Succeeded);  // Fallback 走到恢复分支
    CHECK(forever->halted.load());               // 在途工作被打断
    bool violated = false;
    for (const auto& f : task.failures()) {
      if (f.code == "GUARD_VIOLATED") violated = true;
    }
    CHECK(violated);
  }

  // 2) 进入即违反(fail-closed:键缺失也算违反)→ 子树从未启动
  {
    Engine engine;
    auto forever = std::make_shared<Forever>();
    engine.registerAction("Forever", forever);

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ReactiveGuard while_true="{safe}" period_ms="40">
          <Forever/>
        </ReactiveGuard>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    CHECK(!forever->halted.load());  // 子树从未 RUNNING,谈不上 halt
    bool violated = false;
    for (const auto& f : task.failures()) {
      if (f.code == "GUARD_VIOLATED") violated = true;
    }
    CHECK(violated);
  }

  // 3) 条件持续成立 → 正常完成,巡检不误伤
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    TaskSpec spec;
    spec.slots["safe"] = true;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ReactiveGuard while_true="{safe}" period_ms="40">
          <Ok/>
        </ReactiveGuard>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Succeeded);
  }
}

// ---- M5.3:资源仲裁 ----
static void test_resources() {
  Engine engine;
  engine.registerAction("Forever", std::make_shared<Forever>());
  const char* xml = R"(
    <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)";

  TaskSpec a; a.task_id = "a"; a.resources = {"gripper"};
  auto ta = engine.submitFromText(xml, a);
  CHECK(bool(ta));

  TaskSpec b; b.task_id = "b"; b.resources = {"gripper"};
  auto tb = engine.submitFromText(xml, b);
  CHECK(!tb);  // 冲突拒绝

  TaskSpec c; c.task_id = "c"; c.resources = {"arm"};
  auto tc = engine.submitFromText(xml, c);
  CHECK(bool(tc));  // 无交集可并行

  CHECK(ta.cancel());
  CHECK(ta.waitFor(std::chrono::milliseconds(500)) == TaskState::Canceled);

  TaskSpec d; d.task_id = "d"; d.resources = {"gripper"};
  auto td = engine.submitFromText(xml, d);
  CHECK(bool(td));  // 终态后资源释放

  CHECK(tc.cancel());
  CHECK(td.cancel());
}

// ---- M4.3:watchdog 卡死检测 ----
static void test_watchdog() {
  // 1) 卡死任务:触发回调一次 + halt + TASK_WATCHDOG
  {
    Engine engine;
    auto forever = std::make_shared<Forever>();
    engine.registerAction("Forever", forever);
    std::atomic<int> fired{0};
    engine.setWatchdog(std::chrono::milliseconds(80), true,
                       [&fired](const std::string&, uint64_t quiet_ms) {
                         fired.fetch_add(1);
                         CHECK(quiet_ms >= 80);
                       });
    TaskSpec spec;
    spec.watchdog_ms = 80;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)",
      spec);
    CHECK(task.wait() == TaskState::Failed);
    CHECK(fired.load() == 1);
    CHECK(forever->halted.load());
    auto failures = task.failures();
    CHECK(failures.size() == 1);
    CHECK(failures[0].code == "TASK_WATCHDOG");
  }

  // 2) 只告警不 halt:静默期触发一次,任务由动作完成而成功
  {
    Engine engine;
    class SlowOk : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override { return {}; }
      NodeStatus start(NebulaCtx&, const Done& done) override {
        std::thread([done] {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          done(NodeStatus::SUCCESS, {});
        }).detach();
        return NodeStatus::RUNNING;
      }
    };
    engine.registerAction("SlowOk", std::make_shared<SlowOk>());
    std::atomic<int> fired{0};
    engine.setWatchdog(std::chrono::milliseconds(60), false,
                       [&fired](const std::string&, uint64_t) {
                         fired.fetch_add(1);
                       });
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><SlowOk/></BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(fired.load() == 1);  // 同一静默期只触发一次,不刷屏
  }

  // 3) 持续迁移的任务(重试循环)不误报
  {
    Engine engine;
    engine.registerAction("Failer", std::make_shared<Failer>());
    std::atomic<int> fired{0};
    engine.setWatchdog(std::chrono::milliseconds(150), false,
                       [&fired](const std::string&, uint64_t) {
                         fired.fetch_add(1);
                       });
    TaskSpec spec;
    spec.watchdog_ms = 150;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <RetryWithDelay attempts="2" delay_ms="10" budget_ms="2000">
          <Failer/>
        </RetryWithDelay>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Failed);  // 重试耗尽
    CHECK(fired.load() == 0);                 // 期间节点持续迁移,无静默
  }
}

// ---- 测试加固:边界输入 ----
static void test_boundaries() {
  // 1) Compensable 子树数 != 2 → tick 抛 LogicError,任务 Failed 不崩
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Compensable id="x"><Ok/></Compensable>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    CHECK(task.failures()[0].code == "TICK_EXCEPTION");  // 异常可检索,不只进日志
  }

  // 2) 异步补偿(补偿子树 RUNNING 不休止)→ 1000 tick 触顶 →
  //    COMPENSATION_FAILED,不中断其余补偿,不挂死
  {
    class Stuck : public NebulaAction {
    public:
      BT::PortsList providedPorts() const override { return {}; }
      NodeStatus start(NebulaCtx&, const Done&) override {
        return NodeStatus::RUNNING;
      }
    };
    Engine engine;
    engine.registerAction("SideEffect", std::make_shared<SideEffect>());
    engine.registerAction("Stuck", std::make_shared<Stuck>());
    engine.registerAction("Failer", std::make_shared<Failer>());

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <Compensable id="reserve">
            <SideEffect/>
            <Stuck/>
          </Compensable>
          <Failer/>
        </Sequence>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    bool compensated = false;
    for (const auto& f : task.failures()) {
      if (f.code == "COMPENSATION_FAILED") compensated = true;
    }
    CHECK(compensated);
  }

  // 3) LLM 客户端同步抛异常 → ACTION_EXCEPTION
  {
    Engine engine;
    LlmDecision::Client throwing = [](const nlohmann::json&, Done) {
      throw std::runtime_error("llm endpoint down");
    };
    engine.registerAction("LLMDecision",
                          std::make_shared<LlmDecision>(throwing));
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <LLMDecision prompt="x" branches="a,b" branch="{d}"/>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Failed);
    CHECK(task.failures()[0].code == "ACTION_EXCEPTION");
  }

  // 4) LLM done 重复回调 → 只认第一次(桥的 slot 去重)
  {
    Engine engine;
    auto observed = std::make_shared<std::string>();
    class Observe : public NebulaAction {
    public:
      std::shared_ptr<std::string> out;
      BT::PortsList providedPorts() const override {
        return { BT::InputPort<std::string>("branch") };
      }
      NodeStatus start(NebulaCtx& ctx, const Done&) override {
        *out = ctx.in<std::string>("branch");
        return NodeStatus::SUCCESS;
      }
    };
    auto obs = std::make_shared<Observe>();
    obs->out = observed;
    engine.registerAction("Observe", obs);
    LlmDecision::Client doubleDone = [](const nlohmann::json&, Done done) {
      done(NodeStatus::SUCCESS, nlohmann::json{{"branch", "first"}});
      done(NodeStatus::SUCCESS, nlohmann::json{{"branch", "second"}});
    };
    engine.registerAction("LLMDecision",
                          std::make_shared<LlmDecision>(doubleDone));
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <LLMDecision prompt="x" branches="first,second" branch="{d}"/>
          <Observe branch="{d}"/>
        </Sequence>
      </BehaviorTree></root>)");
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(*observed == "first");
  }

  // 5) journal 损坏头(len 超 64MB 保护上限)→ torn_tail + 前缀保留
  {
    const std::string path = "/tmp/nebulabt_bad_header.bin";
    std::remove(path.c_str());
    {
      JournalFileWriter w;
      CHECK(w.open(path, false));
      JournalEvent e;
      e.task_id = "t1"; e.event = "submitted";
      CHECK(w.append(e));
    }
    {
      std::ofstream f(path, std::ios::app | std::ios::binary);
      // len = 0x7FFFFFFF(超 64MB 上限)+ 4 字节垃圾
      const uint8_t bad[] = {0xFF, 0xFF, 0xFF, 0x7F, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
      f.write(reinterpret_cast<const char*>(bad), sizeof(bad));
    }
    nlohmann::json arr;
    JournalFileReadStats st;
    CHECK(readJournalFile(path, arr, &st));
    CHECK(arr.size() == 1);        // 完好前缀保留
    CHECK(st.torn_tail);           // 损坏头被识别,不会超大分配
    std::remove(path.c_str());
  }

  // 6) ReactiveGuard period_ms=0 → 钳到 1ms,不崩
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    TaskSpec spec;
    spec.slots["safe"] = true;
    spec.schema = nlohmann::json{
        {"slots", {{"safe", {{"type", "bool"}, {"required", true}}}}}};
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ReactiveGuard while_true="{safe}" period_ms="0"><Ok/></ReactiveGuard>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Succeeded);
  }
}

// ---- 1.1.0 收尾:SDK 动态访问 + YAML 清单保真 ----
static void test_dynamic_access_and_yaml() {
  // 1) ctx.inRef / ctx.peek:动态按键访问(CheckSlot 类原语的支撑)
  {
    class Probe : public NebulaAction {
    public:
      std::string refOfKey;     // key 端口绑定的键名
      nlohmann::json peeked;    // peek(key) 的值
      nlohmann::json peekMissing;
      std::string refOfLiteral; // 字面量端口 → 空串
      BT::PortsList providedPorts() const override {
        // 动态键端口必须无类型,避免 {floor} 把真实 int 键钉成 string
        return { BT::InputPort("key", "blackboard key"),
                 BT::InputPort<std::string>("lit") };
      }
      BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
        refOfKey = ctx.inRef("key");
        refOfLiteral = ctx.inRef("lit");
        peeked = ctx.peek(refOfKey);
        peekMissing = ctx.peek("no_such_key");
        return BT::NodeStatus::SUCCESS;
      }
    };
    Engine engine;
    auto probe = std::make_shared<Probe>();
    engine.registerAction("Probe", probe);

    TaskSpec spec;
    spec.slots["floor"] = 3;
    spec.schema = nlohmann::json{
        {"slots", {{"floor", {{"type", "int"}, {"required", true}}}}}};
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Probe key="{floor}" lit="just_text"/>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(probe->refOfKey == "floor");            // {floor} → 键名 "floor"
    CHECK(probe->refOfLiteral.empty());           // 字面量 → 空
    CHECK(probe->peeked == 3);                    // 按名读到真实 int
    CHECK(probe->peekMissing.is_null());          // 缺失键 → null
  }

#ifdef NEBULABT_HAVE_YAML
  // 2) YAML 清单保真:引号标量恒为字符串、版本号不被吞成数值、
  //    bool/int 按真实类型转换(经 loadPack 全链路验证)
  {
    const std::string dir = "/tmp/nebulabt_yaml_pack";
    mkdir("/tmp", 0755);
    mkdir(dir.c_str(), 0755);
    mkdir((dir + "/trees").c_str(), 0755);
    std::ofstream(dir + "/pack.yaml") << R"YAML(
name: yamlpack
version: "1.0.0"          # 引号标量:必须保持字符串
timeout_scalar: 1.0.0     # 无引号但非数值:不得被吞成 1.0
task_types:
  Y_TASK:
    tree: y_tree
    schema:
      slots:
        n: { type: int, required: true }
    failure_codes:
      E_Y: declared in yaml
)YAML";
    std::ofstream(dir + "/trees/y_tree.xml") << R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ParityAction n="{n}" which="{which}"/>
      </BehaviorTree></root>)";

    Engine engine;
    engine.registerAction("ParityAction", std::make_shared<ParityAction>());
    CHECK_NOTHROW(engine.loadPack(dir));   // version 非字符串会在这里抛
    // 版本号保真:经路由清单断言(被吞成 1.0 double 的话 parsePackManifest
    // 会因 missing 'version' 抛异常,loadPack 已覆盖;这里再验证清单)
    bool found = false;
    for (const auto& ti : engine.loadedTaskTypes()) {
      if (ti.task_type == "Y_TASK" && ti.pack == "yamlpack" &&
          ti.version == "1.0.0") {
        found = true;
      }
    }
    CHECK(found);
    // YAML 声明的失败码与 JSON 同等生效
    TaskSpec s;
    s.task_type = "Y_TASK";
    s.slots["n"] = 2;
    auto t = engine.submitTask(s);
    CHECK(t.wait() == TaskState::Succeeded);
  }
#endif
}

// ---- 复审:副作用装饰器 × reactive 父层(BT 语义)----
static void test_reactive_side_effects() {
  class Count : public NebulaAction {
  public:
    std::atomic<int> calls{0};
    BT::PortsList providedPorts() const override { return {}; }
    NodeStatus start(NebulaCtx&, const Done&) override {
      calls.fetch_add(1);
      return NodeStatus::SUCCESS;
    }
  };
  class Wait : public NebulaAction {
  public:
    BT::PortsList providedPorts() const override {
      return { BT::InputPort<int>("ms") };
    }
    NodeStatus start(NebulaCtx& ctx, const Done& done) override {
      std::thread([ms = ctx.in<int>("ms"), done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        done(NodeStatus::SUCCESS, {});
      }).detach();
      return NodeStatus::RUNNING;
    }
  };

  // 1) memory Sequence:Compensable/Checkpoint 各执行恰好一次
  {
    Engine engine;
    auto count = std::make_shared<Count>();
    engine.registerAction("Count", count);
    engine.registerAction("Wait", std::make_shared<Wait>());
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <Sequence>
          <Compensable id="work">
            <Count/>
            <Count/>
          </Compensable>
          <Checkpoint id="cp"><Count/></Checkpoint>
          <Wait ms="30"/>
        </Sequence>
      </BehaviorTree></root>)");
    CHECK(task.waitFor(std::chrono::seconds(1)) == TaskState::Succeeded);
    CHECK(count->calls.load() == 2);  // Compensable 一次 + Checkpoint 一次,无重跑
  }

  // 2) reactive 父层:halt 复位重跑是 BT reactive 语义 → 副作用重复,
  //    加载期必须发警告(提交路径 validateTask 捕获)
  {
    Engine engine;
    auto count = std::make_shared<Count>();
    engine.registerAction("Count", count);
    engine.registerAction("Wait", std::make_shared<Wait>());
    engine.registerAction("Forever", std::make_shared<Forever>());
    std::atomic<int> warns{0};
    engine.setLogger([&warns](LogLevel lv, const std::string& msg) {
      if (lv == LogLevel::Warn && msg.find("reactive parent") != std::string::npos) {
        warns.fetch_add(1);
      }
    });

    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ReactiveSequence>
          <Compensable id="work">
            <Count/>
            <Count/>
          </Compensable>
          <Checkpoint id="cp"><Count/></Checkpoint>
          <Wait ms="30"/>
          <Forever/>
        </ReactiveSequence>
      </BehaviorTree></root>)");
    // tick1:Comp#1 + Cp#1,Wait RUNNING;30ms 后 done 唤醒 →
    // tick2:halt 复位的 Compensable/Checkpoint 重跑(文档化语义),
    // Wait SUCCESS → Forever RUNNING 保持任务活跃
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(task.cancel());
    CHECK(task.waitFor(std::chrono::seconds(1)) == TaskState::Canceled);
    CHECK(warns.load() >= 1);           // 加载期警告可见
    CHECK(count->calls.load() == 4);    // 初次 2 次 + 唤醒重跑 2 次
  }
}

// Probe2:记录黑板原始类型(无 schema 注入规则验证)
class Probe2 : public NebulaAction {
public:
  nlohmann::json flag;
  nlohmann::json rows;
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx& ctx, const Done&) override {
    flag = ctx.peek("flag");
    rows = ctx.peek("rows");
    return NodeStatus::SUCCESS;
  }
};

// ---- backlog:提交原因 / 结构化黑板 / LLM 超时 ----
static void test_backlog_batch() {
  // 1) TaskHandle.error():被拒提交携带可编程的原因
  {
    Engine engine;
    engine.registerAction("Ok", std::make_shared<Ok>());
    engine.registerAction("Forever", std::make_shared<Forever>());

    // 校验失败:原因含首个错误
    TaskSpec bad;
    bad.schema = nlohmann::json{
        {"slots", {{"must", {{"type", "int"}, {"required", true}}}}}};
    bad.slots["wrong_key"] = 1;
    auto t1 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Ok/></BehaviorTree></root>)",
      bad);
    CHECK(!t1);
    CHECK(t1.error().find("validation failed") != std::string::npos);
    CHECK(t1.error().find("first:") != std::string::npos);

    // 无 pack:原因含 task_type
    TaskSpec nopack;
    nopack.task_type = "NO_SUCH_TYPE";
    auto t2 = engine.submitTask(nopack);
    CHECK(!t2);
    CHECK(t2.error().find("no loaded pack provides task_type 'NO_SUCH_TYPE'") !=
          std::string::npos);

    // 资源冲突:原因含资源名
    const char* xml = R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Forever/></BehaviorTree></root>)";
    TaskSpec a; a.task_id = "ra"; a.resources = {"gripper"};
    auto ta = engine.submitFromText(xml, a);
    CHECK(bool(ta));
    TaskSpec b; b.task_id = "rb"; b.resources = {"gripper"};
    auto tb = engine.submitFromText(xml, b);
    CHECK(!tb);
    CHECK(tb.error().find("resource 'gripper'") != std::string::npos);
    CHECK(ta.cancel());
    CHECK(ta.waitFor(std::chrono::milliseconds(500)) == TaskState::Canceled);
  }

  // 2) 结构化黑板:json 数组 slots → ForEach element_json 结构化消费
  {
    Engine engine;
    auto observed = std::make_shared<std::vector<std::string>>();
    class Probe : public NebulaAction {
    public:
      std::shared_ptr<std::vector<std::string>> out;
      BT::PortsList providedPorts() const override {
        return { BT::InputPort<std::string>("sku"),
                 BT::InputPort<nlohmann::json>("item") };
      }
      NodeStatus start(NebulaCtx& ctx, const Done&) override {
        const nlohmann::json item = ctx.in<nlohmann::json>("item");
        out->push_back(ctx.in<std::string>("sku") + ":" +
                       std::to_string(item.value("qty", 0)) + ":" +
                       item.value("kind", "?"));
        return NodeStatus::SUCCESS;
      }
    };
    auto probe = std::make_shared<Probe>();
    probe->out = observed;
    engine.registerAction("Probe", probe);

    TaskSpec spec;
    // schema 声明 json_array:黑板按真实 json 存储
    spec.schema = nlohmann::json{
        {"slots", {{"items", {{"type", "json_array"}, {"required", true}}}}}};
    spec.slots["items"] = nlohmann::json::array({
        nlohmann::json{{"sku", "a1"}, {"qty", 2}, {"kind", "book"}},
        nlohmann::json{{"sku", "b2"}, {"qty", 1}, {"kind", "toy"}},
    });
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <ForEach array="{items}" element="{item_s}" element_json="{item}">
          <Probe sku="{item_s}" item="{item}"/>
        </ForEach>
      </BehaviorTree></root>)", spec);
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(observed->size() == 2);
    CHECK((*observed)[0] == "a1:2:book");
    CHECK((*observed)[1] == "b2:1:toy");
  }

  // 3) 无 schema 注入升级:bool 真实类型 + 对象数组真实 json
  {
    Engine engine;
    auto probe = std::make_shared<Probe2>();
    engine.registerAction("Probe2", probe);
    TaskSpec spec;
    spec.slots["flag"] = true;                      // 旧规则 dump 成 "true"
    spec.slots["rows"] = nlohmann::json::array({   // 旧规则逐元素 dump
        nlohmann::json{{"k", "v"}}});
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t"><Probe2/></BehaviorTree></root>)",
      spec);
    CHECK(task.wait() == TaskState::Succeeded);
    CHECK(probe->flag.is_boolean() && probe->flag.get<bool>());
    CHECK(probe->rows.is_array() && probe->rows[0]["k"] == "v");
  }

  // 4) LLM 节点级超时:挂起客户端 → LLM_TIMEOUT;timeout_ms=0 关闭
  {
    Engine engine;
    LlmDecision::Client hanging = [](const nlohmann::json&, Done) {
      std::this_thread::sleep_for(std::chrono::seconds(10));  // 永不 done
    };
    engine.registerAction("LLMDecision",
                          std::make_shared<LlmDecision>(hanging));

    TaskSpec spec;
    spec.slots["situation"] = "x";
    spec.schema = nlohmann::json{
        {"slots", {{"situation", {{"type", "string"}, {"required", true}}}}}};
    auto t1 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <LLMDecision prompt="{situation}" branches="a,b" branch="{d}"
                     timeout_ms="120"/>
      </BehaviorTree></root>)", spec);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(t1.wait() == TaskState::Failed);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() <
          3000);
    CHECK(t1.failures()[0].code == "LLM_TIMEOUT");

    // timeout_ms=0:关闭节点级超时 → 任务保持 RUNNING,由取消收尾
    auto t2 = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <LLMDecision prompt="{situation}" branches="a,b" branch="{d}"
                     timeout_ms="0"/>
      </BehaviorTree></root>)", spec);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(t2.state() == TaskState::Running);
    CHECK(t2.cancel());
  }
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
  test_while_loop();
  std::cout << "ok  while loop\n";
  test_switch_n();
  std::cout << "ok  switch n\n";
  test_journal_events();
  std::cout << "ok  journal events\n";
  test_checkpoint_resume();
  std::cout << "ok  checkpoint resume\n";
  test_checkpoint_resume_across_engines();
  std::cout << "ok  checkpoint resume across engines\n";
  test_explain_timing();
  std::cout << "ok  explain timing\n";
  test_pack_and_canary();
  std::cout << "ok  pack loading + canary routing\n";
  test_scripted_mock();
  std::cout << "ok  scripted mock\n";
  test_template_and_requires();
  std::cout << "ok  template override + requires\n";
  test_checkpoint_in_loop();
  std::cout << "ok  checkpoint inside loop (no false skip)\n";
  test_journal_file();
  std::cout << "ok  journal file (CRC + torn tail tolerance)\n";
  test_limits();
  std::cout << "ok  resource limits (queue + active tasks)\n";
  test_watchdog();
  std::cout << "ok  watchdog (stuck task detection)\n";
  test_compensable();
  std::cout << "ok  compensable (saga compensation in reverse order)\n";
  test_reactive_guard();
  std::cout << "ok  reactive guard (violation halts subtree)\n";
  test_resources();
  std::cout << "ok  resource arbitration (conflict rejected)\n";
  test_virtual_clock();
  std::cout << "ok  virtual clock (retry/timeout/watchdog instant)\n";
  test_m7_observability();
  std::cout << "ok  m7 observability (failure inputs / runtime explain / groot2)\n";
  test_m8_agent();
  std::cout << "ok  m8 agent (failure code registry / LLM decision guardrails)\n";
  test_review_fixes();
  std::cout << "ok  review fixes (explain safety / logf reentry / crc constexpr /\n"
               "     advance guard / zombie submit / journal cap)\n";
  test_boundaries();
  std::cout << "ok  boundaries (comp children / async compensation / llm throw /\n"
               "     double done / bad journal header / guard period=0)\n";
  test_dynamic_access_and_yaml();
  std::cout << "ok  dynamic access (inRef/peek) + yaml manifest fidelity\n";
  test_reactive_side_effects();
  std::cout << "ok  reactive side effects (memory=once; reactive reruns + warning)\n";
  std::cout << "ALL TESTS PASSED\n";
  return 0;
}
