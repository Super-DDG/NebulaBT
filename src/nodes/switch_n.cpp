#include "nebulabt/nodes/switch_n.hpp"

namespace nebulabt {

SwitchNNode::SwitchNNode(const std::string& name, const BT::NodeConfig& config)
  : BT::ControlNode(name, config) {}

BT::PortsList SwitchNNode::providedPorts() {
  BT::PortsList ports;
  ports.insert(BT::InputPort<std::string>("variable"));
  for (int i = 1; i <= kMaxCases; ++i) {
    ports.insert(BT::InputPort<std::string>(
        std::string("case_") + std::to_string(i)));
  }
  return ports;
}

BT::NodeStatus SwitchNNode::tick() {
  const size_t numChildren = children().size();
  if (numChildren < 2) {
    throw BT::LogicError("SwitchN must have at least 2 children "
                         "(1 case + 1 default)");
  }
  const size_t numCases = numChildren - 1;

  // 命中匹配的 case;全部未命中走 default(最后一个子节点)
  size_t matchIndex = numCases;
  auto varRes = getInput<std::string>("variable");
  if (varRes) {
    const std::string& variable = varRes.value();
    for (size_t i = 0; i < numCases; ++i) {
      auto caseRes =
          getInput<std::string>(std::string("case_") + std::to_string(i + 1));
      if (caseRes && caseRes.value() == variable) {
        matchIndex = i;
        break;
      }
    }
  }

  // 切换分支时 halt 上一个 RUNNING 子节点
  if (running_child_ >= 0 &&
      static_cast<size_t>(running_child_) != matchIndex) {
    haltChild(static_cast<size_t>(running_child_));
  }

  const BT::NodeStatus ret = children()[matchIndex]->executeTick();
  if (ret == BT::NodeStatus::RUNNING) {
    running_child_ = static_cast<int>(matchIndex);
  } else {
    resetChildren();
    running_child_ = -1;
  }
  return ret;
}

void SwitchNNode::halt() {
  running_child_ = -1;
  ControlNode::halt();
}

} // namespace nebulabt
