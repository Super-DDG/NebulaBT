#include "nebulabt/sdk/action.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/tree_node.h>

namespace nebulabt {

NebulaCtx::NebulaCtx(BT::TreeNode& node, std::shared_ptr<FailureLog> failures,
                     std::string taskId, std::string taskType,
                     std::shared_ptr<FailureCodeTable> codes)
  : node_(&node), failures_(std::move(failures)), codes_(std::move(codes)),
    task_id_(std::move(taskId)), task_type_(std::move(taskType)) {}

void NebulaCtx::fail(std::string code, std::string message) {
  if (!failures_) {
    return;
  }
  FailureRecord r;
  r.code = code;
  r.node = node_->name();
  r.node_type = node_->registrationName();
  r.message = std::move(message);
  r.timestamp = nowTimestamp();
  // M7 富化:自动附带激活时刻的输入端口快照(排障不用猜"当时参数")
  r.inputs = inputs_;
  // M8 登记表:登记表启用(非空)且未声明 => 标记,explain 可筛出拼写错误
  r.undeclared_code = codes_ && !codes_->known(code);
  failures_->add(std::move(r));
}

} // namespace nebulabt
