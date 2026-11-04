#include "nebulabt/nodes/while_loop.hpp"

namespace nebulabt {

WhileLoopNode::WhileLoopNode(const std::string& name,
                             const BT::NodeConfig& config)
  : BT::ControlNode(name, config) {}

BT::PortsList WhileLoopNode::providedPorts() {
  return {
    BT::InputPort<int>("max_iterations", 1000,
        "单次激活内 body 最大成功次数(防全同步死循环),0 = 不限"),
  };
}

BT::NodeStatus WhileLoopNode::tick() {
  if (children().size() != 2) {
    throw BT::LogicError(
        "WhileLoop must have exactly 2 children (condition + body)");
  }
  if (status() == BT::NodeStatus::IDLE) {
    body_running_ = false;
    iterations_ = 0;
  }
  const int maxIterations = getInput<int>("max_iterations").value_or(1000);

  while (true) {
    BT::TreeNode* cond = children()[0];
    BT::TreeNode* body = children()[1];

    // body 未在运行:评估 condition 决定是否进入/继续
    if (!body_running_) {
      const BT::NodeStatus cs = cond->executeTick();
      if (cs == BT::NodeStatus::RUNNING) {
        return BT::NodeStatus::RUNNING;  // 条件自身异步:等它完成
      }
      if (cs == BT::NodeStatus::FAILURE) {
        resetChildren();                 // 循环正常结束
        return BT::NodeStatus::SUCCESS;
      }
    }

    // 执行 body
    const BT::NodeStatus bs = body->executeTick();
    if (bs == BT::NodeStatus::RUNNING) {
      body_running_ = true;
      return BT::NodeStatus::RUNNING;
    }

    body_running_ = false;
    resetChildren();  // 关键:子节点回到 IDLE,否则下次 executeTick 缓存旧终态
    if (bs == BT::NodeStatus::FAILURE) {
      return BT::NodeStatus::FAILURE;    // 异常退出
    }

    // body SUCCESS:同 tick 重估 condition 继续循环(事件驱动模型要求);
    // max_iterations 防全同步死循环
    if (maxIterations > 0 && ++iterations_ >= maxIterations) {
      return BT::NodeStatus::FAILURE;
    }
  }
}

void WhileLoopNode::halt() {
  body_running_ = false;
  iterations_ = 0;
  ControlNode::halt();
}

} // namespace nebulabt
