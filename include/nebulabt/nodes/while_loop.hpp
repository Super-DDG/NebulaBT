#pragma once
/// WhileLoop:真正的 while 循环(ControlNode),2 个子节点:condition + body。
///
///   <WhileLoop max_iterations="1000">
///     <CheckHasWork/>    <!-- condition:同步条件节点 -->
///     <DoOneUnit/>       <!-- body:处理一个单元 -->
///   </WhileLoop>
///
/// 语义:
/// - body RUNNING 期间不评估 condition(非 reactive,避免重复检测误退出)
/// - body SUCCESS 后重新评估 condition:SUCCESS 继续、FAILURE 正常退出(SUCCESS)
/// - body FAILURE -> 整体 FAILURE(异常退出)
/// - condition FAILURE -> 整体 SUCCESS(循环正常结束)
///
/// 与 BT.CPP 内置 WhileDoElse 的区别:后者是 reactive if-then-else,
/// body SUCCESS 后直接返回 SUCCESS,不循环。
///
/// 事件驱动适配(与轮询式运行时的关键差异):
/// - body SUCCESS 后在**同一 tick 内**重估 condition 并继续——本框架无轮询,
///   若像轮询式实现那样"返回 RUNNING 等下一 tick",没有事件就永远不会再 tick
/// - max_iterations(默认 1000)防止"全同步 + 条件永真"导致的失控死循环
///
/// 典型用法:循环处理物品直到检测不到、按队列逐单处理直到队列为空。

#include <behaviortree_cpp/control_node.h>
#include <string>

namespace nebulabt {

class WhileLoopNode : public BT::ControlNode {
public:
  WhileLoopNode(const std::string& name, const BT::NodeConfig& config);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  bool body_running_ = false;
  int iterations_ = 0;
};

} // namespace nebulabt
