#pragma once
/// BridgeNode —— NebulaAction 与 BT.CPP 的桥(engine 内部)。
///
/// 端口声明来自 action 实例(运行期),经
///   factory.registerNodeType<BridgeNode>(id, ports, action, sched, core)
/// 注册(BT.CPP 4.x 支持显式传 PortsList)。
///
/// 线程模型:onStart/onRunning 只在调度线程执行;done 可在任意线程调用,
/// 结果经 Slot(共享指针,生命周期独立于节点)暂存,再 post 唤醒调度器。

#include "nebulabt/core/scheduler.hpp"
#include "nebulabt/sdk/action.hpp"
#include <behaviortree_cpp/action_node.h>
#include <memory>
#include <string>

namespace nebulabt {
namespace detail {

struct EngineCore;

class BridgeNode : public BT::StatefulActionNode {
public:
  BridgeNode(const std::string& name, const BT::NodeConfig& config,
             NebulaActionPtr action, Scheduler* scheduler, EngineCore* core);

  static BT::PortsList providedPorts();

  BT::NodeStatus onStart() override;
  BT::NodeStatus onRunning() override;
  void onHalted() override;

private:
  struct PendingResult;
  struct Slot;
  std::unique_ptr<PendingResult> takePending();
  void clearPending();
  void maybeAutoRecordFailure();
  void initCtx();
  Done makeDone();

  NebulaActionPtr action_;
  Scheduler* scheduler_;
  EngineCore* core_;
  std::shared_ptr<Slot> slot_;
  std::unique_ptr<NebulaCtx> ctx_;
  std::string task_id_;
  size_t failCountAtStart_ = 0;
};

} // namespace detail
} // namespace nebulabt
