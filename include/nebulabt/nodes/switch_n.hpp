#pragma once
/// SwitchN:可变 case 数的 Switch(ControlNode)。
///
/// BT.CPP 4.9 内置 Switch2~Switch6(模板限定 case 上限);SwitchN 去掉上限,
/// case 数 = 子节点数 - 1(default 恒为最后一个子节点):
///
///   <SwitchN variable="{mode}" case_1="a" case_2="b" case_3="c">
///     <ActionA/>   <!-- case_1 -->
///     <ActionB/>   <!-- case_2 -->
///     <ActionC/>   <!-- case_3 -->
///     <Default/>   <!-- 全部未命中时执行 -->
///   </SwitchN>
///
/// 语义(与内置 SwitchNode 一致):
/// - variable 与 case_i 字符串等值比较
/// - 切换分支时 halt 上一个 RUNNING 子节点
/// - variable 缺失/全部未命中 -> 执行 default(最后一个子节点)

#include <behaviortree_cpp/control_node.h>
#include <string>
#include <vector>

namespace nebulabt {

class SwitchNNode : public BT::ControlNode {
public:
  SwitchNNode(const std::string& name, const BT::NodeConfig& config);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  static constexpr int kMaxCases = 32;
  int running_child_ = -1;
};

} // namespace nebulabt
