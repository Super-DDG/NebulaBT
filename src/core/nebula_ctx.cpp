#include "nebulabt/sdk/action.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/tree_node.h>

namespace nebulabt {

NebulaCtx::NebulaCtx(BT::TreeNode& node, std::shared_ptr<FailureLog> failures,
                     std::string taskId, std::string taskType)
  : node_(&node), failures_(std::move(failures)),
    task_id_(std::move(taskId)), task_type_(std::move(taskType)) {}

void NebulaCtx::fail(std::string code, std::string message) {
  if (!failures_) {
    return;
  }
  FailureRecord r;
  r.code = std::move(code);
  r.node = node_->name();
  r.node_type = node_->registrationName();
  r.message = std::move(message);
  r.timestamp = nowTimestamp();
  // M7 富化:自动附带激活时刻的输入端口快照(排障不用猜"当时参数")
  r.inputs = inputs_;
  failures_->add(std::move(r));
}

} // namespace nebulabt
