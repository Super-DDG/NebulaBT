// ============================================================================
// NebulaBT 核心性能基准(嵌入式选型数据)
//
//   B1 树创建+校验   validate(15 节点树,含 L2 数据流检查)  µs/次
//   B2 任务周转      submit + wait(含创建)                 µs/任务
//   B3 树内 tick     WhileLoop 单任务内 10k 次迭代          ns/tick
//   B4 journal 开销  帧落盘 sink vs 无 sink                  ns/事件
//   B5 explain       终态树遍历(15 节点)                   µs/次
//   内存             进程峰值 RSS(VmHWM)
//
// 纯稳态单线程测量;结果与机器相关,基线记录见 docs/benchmarks.md。
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/journal_file.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace nebulabt;
using BT::NodeStatus;
using Clock = std::chrono::steady_clock;

class Ok : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override { return NodeStatus::SUCCESS; }
};

class Failer : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.fail("E_BENCH", "by design");
    return NodeStatus::FAILURE;
  }
};

// 计数条件:前 N 次 SUCCESS,之后 FAILURE(驱动 WhileLoop 恰好 N 轮)
class CountCond : public NebulaAction {
public:
  std::atomic<int> n{0};
  int limit = 0;
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override {
    return n.fetch_add(1) < limit ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
  }
};

static double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void report(const std::string& name, int iters, double ms) {
  std::printf("  %-30s %10.2f us/op  %10.0f ops/s\n", name.c_str(),
              ms * 1000.0 / iters, iters / (ms / 1000.0));
}

static long peakRssKb() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      return std::atol(line.c_str() + 6);
    }
  }
  return -1;
}

#define CHECK_STATE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "bench state check failed: " #cond "\n";                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)

// 15 节点中等树(分支 + 重试 + 循环 + 检查点,贴近真实业务密度)
static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="biz">
  <BehaviorTree ID="biz">
    <Sequence>
      <Ok/>
      <Fallback>
        <Failer/>
        <Ok/>
      </Fallback>
      <RetryWithDelay attempts="1" delay_ms="1" budget_ms="100">
        <Ok/>
      </RetryWithDelay>
      <ForEach array="{items}" element="{sku}">
        <Ok/>
      </ForEach>
      <Checkpoint id="bench_cp">
        <Ok/>
      </Checkpoint>
      <SwitchN variable="{tier}" case_1="express">
        <Ok/>
        <Ok/>
      </SwitchN>
      <Ok/>
    </Sequence>
  </BehaviorTree>
</root>)";

int main() {
  Engine engine;
  engine.registerAction("Ok", std::make_shared<Ok>());
  engine.registerAction("Failer", std::make_shared<Failer>());
  auto cond = std::make_shared<CountCond>();
  engine.registerAction("Cond", cond);
  engine.setLogger([](LogLevel, const std::string&) {});  // 静音任务日志

  TaskSpec spec;
  spec.slots["items"] = {"a", "b", "c"};
  spec.slots["tier"] = "standard";

  std::printf("NebulaBT core benchmark (single-thread steady state)\n\n");

  // B1 树创建 + 加载期校验(validate 恒开 L2 数据流检查:
  // {items}/{tier} 须在 schema 声明)
  const nlohmann::json schema = nlohmann::json{
      {"slots",
       {{"items", {{"type", "string"}, {"required", true}}},
        {"tier", {{"type", "string"}, {"required", true}}}}}};
  {
    constexpr int N = 2000;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
      const auto errors = engine.validate(kXml, schema);
      if (!errors.empty()) {
        std::cerr << "unexpected validation errors\n";
        return 1;
      }
    }
    report("B1 tree create+validate (15n)", N, msSince(t0));
  }

  // B2 任务周转(submit -> 终态,含树创建;事件驱动全链路)
  {
    constexpr int N = 2000;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
      auto task = engine.submitFromText(kXml, spec);
      if (task.wait() != TaskState::Succeeded) {
        std::cerr << "unexpected task failure\n";
        return 1;
      }
    }
    report("B2 task turnaround (15n)", N, msSince(t0));
  }

  // B3 树内 tick 吞吐(单任务,WhileLoop 10k 轮,每轮 2 次节点 tick)
  {
    constexpr int N = 10000;
    cond->n.store(0);
    cond->limit = N;
    auto task = engine.submitFromText(R"(
      <root BTCPP_format="v4"><BehaviorTree ID="t">
        <WhileLoop max_iterations="20000">
          <Cond/>
          <Ok/>
        </WhileLoop>
      </BehaviorTree></root>)");
    auto t0 = Clock::now();
    if (task.wait() != TaskState::Succeeded) {
      std::cerr << "loop task failed\n";
      return 1;
    }
    const double ms = msSince(t0);
    std::printf("  %-30s %10.1f ns/tick %10.0f ticks/s\n",
                "B3 in-tree tick (2-node loop)",
                ms * 1e6 / (N * 2), (N * 2) / (ms / 1000.0));
  }

  // B4 journal 帧落盘吞吐(纯 append,含 CRC32 + fflush;
  // 端到端差值法被任务周转方差淹没,故直测)
  {
    constexpr int N = 20000;
    JournalEvent e;
    e.task_id = "bench";
    e.task_type = "BENCH";
    e.event = "checkpoint";
    e.data = nlohmann::json{
        {"id", "cp1"},
        {"blackboard",
         nlohmann::json{{"sku", "a1"}, {"qty", 3}, {"paid", true}}}};
    const std::string path = "/tmp/nebulabt_bench_journal.bin";
    std::remove(path.c_str());
    nebulabt::JournalFileWriter w;
    CHECK_STATE(w.open(path, false));
    const size_t bytes = nlohmann::json(e).dump().size() + 8;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
      CHECK_STATE(w.append(e));
    }
    const double ms = msSince(t0);
    w.close();
    std::remove(path.c_str());
    std::printf("  %-30s %10.0f ns/ev  %8.1f MB/s\n",
                "B4 journal frame append", ms * 1e6 / N,
                bytes * N / (ms / 1000.0) / 1e6);
  }

  // B5 explain(终态,直接树遍历)
  {
    auto task = engine.submitFromText(kXml, spec);
    task.wait();
    constexpr int N = 5000;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
      const auto j = task.explain();
      if (j.empty()) {
        return 1;
      }
    }
    report("B5 explain (finished, 15n)", N, msSince(t0));
  }

  std::printf("\npeak RSS: %ld kB\n", peakRssKb());
  return 0;
}
