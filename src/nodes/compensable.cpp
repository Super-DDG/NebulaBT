#include "nebulabt/nodes/compensable.hpp"

#include <behaviortree_cpp/blackboard.h>

namespace nebulabt {

CompensableNode::CompensableNode(const std::string& name,
                                 const BT::NodeConfig& config, ISagaHost* host)
  : BT::ControlNode(name, config), host_(host) {}

BT::PortsList CompensableNode::providedPorts() {
  return {
    BT::InputPort<std::string>("id", "补偿单元标识;journal 与补偿日志按它追溯"),
  };
}

BT::NodeStatus CompensableNode::tick() {
  if (children().size() != 2) {
    throw BT::LogicError(
        "Compensable must have exactly 2 children (main work + compensation)");
  }
  if (status() == BT::NodeStatus::IDLE) {
    saga_id_ = getInput<std::string>("id").value_or("");
    if (saga_id_.empty()) {
      throw BT::RuntimeError("Compensable: 'id' port is required");
    }
    task_id_ = "unknown";
    if (auto bb = config().blackboard) {
      bb->get("task_id", task_id_);
    }
  }

  BT::TreeNode* main = children()[0];
  BT::TreeNode* comp = children()[1];
  (void)comp;  // 补偿子树正常运行永不执行,只在 executeCompensation 里 tick

  const BT::NodeStatus st = main->executeTick();
  if (st == BT::NodeStatus::SUCCESS) {
    if (host_) {
      host_->sagaPush(task_id_, this, saga_id_);
    }
    resetChildren();  // 回到 IDLE:循环重入/重 tick 不缓存旧终态(WhileLoop 教训)
  }
  return st;
}

BT::NodeStatus CompensableNode::executeCompensation() {
  BT::TreeNode* comp = children()[1];
  haltChild(1);  // 确保从 IDLE 起步(TreeNode::halt 为私有,走 ControlNode 接口)
  BT::NodeStatus st = BT::NodeStatus::IDLE;
  for (int i = 0; i < kMaxCompensationTicks; ++i) {
    st = comp->executeTick();
    if (st != BT::NodeStatus::RUNNING) {
      break;
    }
  }
  haltChild(1);
  return st;
}

} // namespace nebulabt
