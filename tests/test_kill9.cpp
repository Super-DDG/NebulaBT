// ============================================================================
// kill -9 实机恢复测试(POSIX;M2 验收项)
//
// 子进程:跑一个带 Checkpoint 的任务,journal 经 sink 逐行落盘,
//         任务卡在长耗时动作上;父进程在检查点确认落盘后 SIGKILL 子进程。
// 父进程:读回 journal 文件,恢复提交同一棵树——
//         副作用(副作用计数)不重复执行,任务完成。
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace nebulabt;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

class SideEffect : public NebulaAction {
public:
  std::atomic<int> calls{0};
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    calls.fetch_add(1);
    return BT::NodeStatus::SUCCESS;
  }
};

// 长耗时动作:sleepMs 毫秒后成功(子进程里等不到被杀)
class LongAction : public NebulaAction {
public:
  explicit LongAction(int sleepMs) : sleep_ms_(sleepMs) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done& done) override {
    std::thread([sleep = sleep_ms_, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }

private:
  int sleep_ms_;
};

static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="t">
  <BehaviorTree ID="t">
    <Sequence>
      <Checkpoint id="cp1">
        <SideEffect/>
      </Checkpoint>
      <LongAction/>
    </Sequence>
  </BehaviorTree>
</root>)";

// 子进程:跑任务直到被杀;journal 逐行写入 journalPath
static int runChild(const std::string& journalPath) {
  Engine engine;
  engine.registerAction("SideEffect", std::make_shared<SideEffect>());
  engine.registerAction("LongAction", std::make_shared<LongAction>(30000));

  std::ofstream journalFile(journalPath, std::ios::app);
  if (!journalFile) {
    return 43;
  }
  engine.setJournalSink([&](const JournalEvent& e) {
    journalFile << nlohmann::json(e).dump() << std::endl;
  });

  auto task = engine.submitFromText(kXml);
  task.wait();  // 会被 SIGKILL 打断(永远等不到)
  return 0;
}

int main() {
  const std::string journalPath = "/tmp/nebulabt_kill9_test.jsonl";
  std::remove(journalPath.c_str());

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // 子进程:任何异常直接退出非零,不得返回到测试框架
    _exit(runChild(journalPath));
  }

  // 父进程:等子进程记录检查点(= 1 次 SideEffect + checkpoint 事件落盘)
  bool hasCheckpoint = false;
  for (int i = 0; i < 100 && !hasCheckpoint; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ifstream f(journalPath);
    std::string line;
    while (std::getline(f, line)) {
      if (line.find("\"checkpoint\"") != std::string::npos) {
        hasCheckpoint = true;
      }
    }
  }
  CHECK(hasCheckpoint);  // 检查点已落盘(此时长动作仍在 RUNNING)

  // kill -9
  CHECK(kill(pid, SIGKILL) == 0);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  // 恢复:读回 journal,新 Engine 提交同一棵树
  Engine engine;
  auto side = std::make_shared<SideEffect>();
  engine.registerAction("SideEffect", side);
  engine.registerAction("LongAction", std::make_shared<LongAction>(50));

  TaskSpec spec;
  std::ifstream f(journalPath);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    try {
      spec.resume_journal.push_back(nlohmann::json::parse(line));
    } catch (const std::exception& e) {
      // 崩溃瞬间可能写出行尾不完整的行:容错跳过(见 DESIGN 已知限制)
      std::cerr << "skip incomplete journal line: " << e.what() << "\n";
    }
  }
  CHECK(spec.resume_journal.size() >= 3);  // submitted/started/checkpoint

  auto task = engine.submitFromText(kXml, spec);
  CHECK(task.wait() == TaskState::Succeeded);
  CHECK(side->calls.load() == 0);  // 副作用在恢复进程中零重复执行!

  std::cout << "ok  kill -9 resume (checkpoint skipped, side effect preserved)\n";
  std::cout << "ALL KILL9 TESTS PASSED\n";
  return 0;
}
