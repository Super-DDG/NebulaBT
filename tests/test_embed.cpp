// ============================================================================
// 树嵌入二进制测试(M7):CMake 生成嵌入头,无文件系统依赖提交。
// 验收:嵌入字符串可解析、可执行、行为与从文件提交一致。
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include "embed_me.h"  // nebulabt_embed_trees 生成

#include <iostream>

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
  std::atomic<int> calls{0};
  BT::PortsList providedPorts() const override { return {}; }
  NodeStatus start(NebulaCtx&, const Done&) override {
    calls.fetch_add(1);
    return NodeStatus::SUCCESS;
  }
};

int main() {
  Engine engine;
  auto ok = std::make_shared<Ok>();
  engine.registerAction("Ok", ok);

  // 嵌入树:无文件系统也能提交(编译进二进制的字符串常量)
  auto task = engine.submitFromText(nebulabt_embed::NEBULA_TREE_EMBED_ME);
  CHECK(task.wait() == TaskState::Succeeded);
  CHECK(ok->calls.load() == 2);

  std::cout << "ALL EMBED TESTS PASSED\n";
  return 0;
}
