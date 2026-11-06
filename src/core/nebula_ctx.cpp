#include "nebulabt/sdk/action.hpp"

#include "nebulabt/core/any_json.hpp"
#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/tree_node.h>

namespace nebulabt {

NebulaCtx::NebulaCtx(BT::TreeNode& node, std::shared_ptr<FailureLog> failures,
                     std::string taskId, std::string taskType,
                     std::shared_ptr<FailureCodeTable> codes)
  : node_(&node), failures_(std::move(failures)), codes_(std::move(codes)),
    task_id_(std::move(taskId)), task_type_(std::move(taskType)) {}

std::string NebulaCtx::inRef(const std::string& port) const {
  // 经 const 引用选择 public 的 const config() 重载(非 const 版为 protected)
  const BT::TreeNode& node = *node_;
  const auto& ports = node.config().input_ports;
  auto it = ports.find(port);
  if (it == ports.end()) {
    return "";
  }
  const std::string& remap = it->second;
  if (remap.size() >= 3 && remap.front() == '{' && remap.back() == '}') {
    const std::string key = remap.substr(1, remap.size() - 2);
    const auto first = key.find_first_not_of(" \t");
    if (first == std::string::npos) {
      return "";
    }
    const auto last = key.find_last_not_of(" \t");
    return key.substr(first, last - first + 1);
  }
  return "";
}

nlohmann::json NebulaCtx::peek(const std::string& blackboardKey) const {
  const BT::TreeNode& node = *node_;
  if (auto bb = node.config().blackboard) {
    if (auto entry = bb->getAnyLocked(blackboardKey)) {
      return nebulabt::anyToJson(entry);
    }
  }
  return nullptr;
}

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
