#include "nebulabt/nodes/for_each.hpp"

namespace nebulabt {

ForEachNode::ForEachNode(const std::string& name, const BT::NodeConfig& config)
  : BT::ControlNode(name, config) {}

BT::PortsList ForEachNode::providedPorts() {
  return {
    BT::InputPort<std::vector<std::string>>("array",
        "要遍历的集合:vector<string> 黑板值,或 ';' 分隔字符串"),
    BT::OutputPort<std::string>("element", "当前元素(写入黑板)"),
    BT::InputPort<bool>("continue_on_failure", false,
        "子节点失败时跳过该元素继续,而非整体失败"),
  };
}

BT::NodeStatus ForEachNode::tick() {
  if (children().size() != 1) {
    throw BT::RuntimeError("ForEach: requires exactly one child");
  }
  if (status() == BT::NodeStatus::IDLE) {
    auto arr = getInput<std::vector<std::string>>("array");
    if (!arr) {
      throw BT::RuntimeError("ForEach: invalid/missing 'array' port: ",
                             arr.error());
    }
    items_ = *arr;
    index_ = 0;
  }

  const bool continueOnFailure = getInput<bool>("continue_on_failure").value_or(false);

  while (index_ < items_.size()) {
    auto setOk = setOutput("element", items_[index_]);
    if (!setOk) {
      throw BT::RuntimeError("ForEach: 'element' port must be remapped: ",
                             setOk.error());
    }
    const BT::NodeStatus childStatus = children()[0]->executeTick();
    if (childStatus == BT::NodeStatus::RUNNING) {
      return BT::NodeStatus::RUNNING;
    }
    if (childStatus == BT::NodeStatus::FAILURE && !continueOnFailure) {
      haltChild(0);
      return BT::NodeStatus::FAILURE;
    }
    haltChild(0); // 重置子节点,进入下一个元素
    ++index_;
  }
  return BT::NodeStatus::SUCCESS;
}

void ForEachNode::halt() {
  index_ = 0;
  ControlNode::halt();
}

} // namespace nebulabt
