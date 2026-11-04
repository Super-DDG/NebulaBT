#pragma once
/// ForEach:动态树原语——针对"业务逻辑按数据集合展开"的场景。
/// (动机见 docs/DESIGN.md;BT.CPP 的 XML 树是静态的,集合遍历
///  一直是行为树进入业务领域的最大阻力之一。)
///
/// 遍历 array,每轮将当前元素写入 element 输出端口,依次 tick 子节点:
/// - 子节点 SUCCESS -> 处理下一个元素
/// - 子节点 RUNNING -> 返回 RUNNING(下轮从当前元素继续)
/// - 子节点 FAILURE -> 默认整体 FAILURE;continue_on_failure=true 时跳过继续
/// 空数组直接 SUCCESS。
///
/// array 支持 vector<string> 黑板值,或 ";" 分隔的字符串。

#include <behaviortree_cpp/control_node.h>
#include <string>
#include <vector>

namespace nebulabt {

class ForEachNode : public BT::ControlNode {
public:
  ForEachNode(const std::string& name, const BT::NodeConfig& config);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  std::vector<std::string> items_;
  size_t index_ = 0;
};

} // namespace nebulabt
