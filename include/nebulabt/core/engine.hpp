#pragma once
/// NebulaBT 引擎:任务生命周期 + 事件驱动调度 + 结构化失败。
/// 分层与子系统职责见 docs/DESIGN.md。
///
/// 用法:
///   Engine engine;
///   engine.registerAction("MyNode", std::make_shared<MyAction>());
///   auto task = engine.submitFromText(xml, spec);
///   task.wait();
///   task.explain();

#include "nebulabt/core/task.hpp"
#include "nebulabt/sdk/action.hpp"
#include <functional>
#include <memory>
#include <string>

namespace BT {
class Tree; // 前向声明(全局 ::BT),仅用于 Engine 私有成员的 shared_ptr
}

namespace nebulabt {

enum class LogLevel { Info, Warn, Error };

class Engine {
public:
  /// 任务终态回调(在调度线程执行,应当快速返回)
  using FinishedCallback = std::function<void(TaskHandle)>;
  using LogCallback = std::function<void(LogLevel, const std::string&)>;

  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// 注册业务动作;registrationId 即树 XML 中的节点标签。
  /// 端口声明取自 action 实例(运行期),经 BT.CPP 的
  /// registerNodeType(id, ports, args...) 显式注册。
  void registerAction(const std::string& registrationId, NebulaActionPtr action);

  TaskHandle submitFromText(const std::string& xmlText, TaskSpec spec = {});
  TaskHandle submitFromFile(const std::string& xmlPath, TaskSpec spec = {});

  void onFinished(FinishedCallback cb);

  /// 日志回调(默认输出到 stderr)
  void setLogger(LogCallback cb);

private:
  TaskHandle submitTree(std::shared_ptr<BT::Tree> tree, TaskSpec spec);

  std::shared_ptr<detail::EngineCore> core_;
};

} // namespace nebulabt
