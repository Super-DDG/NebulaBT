#include "node_stats.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/decorator_node.h>
#include <behaviortree_cpp/tree_node.h>
#include <chrono>
#include <cmath>

namespace nebulabt {
namespace detail {

TaskTraceLogger::TaskTraceLogger(BT::TreeNode* root,
                                 std::shared_ptr<NodeStatsMap> stats,
                                 std::shared_ptr<ActivityStamp> activity,
                                 std::shared_ptr<NebulaClock> clock)
  : BT::StatusChangeLogger(root), stats_(std::move(stats)),
    activity_(std::move(activity)), clock_(std::move(clock)) {
  setTimestampType(BT::TimestampType::relative);
}

void TaskTraceLogger::callback(BT::Duration timestamp, const BT::TreeNode& node,
                               BT::NodeStatus prev, BT::NodeStatus status) {
  const double ms =
      std::chrono::duration<double, std::milli>(timestamp).count();
  if (activity_) {
    activity_->store(static_cast<int64_t>(
        clock_ ? clock_->nowMs()
               : std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count()));
  }
  std::lock_guard<std::mutex> lk(stats_->mu);
  auto& st = stats_->by_path[node.fullPath()];
  if (st.first_ms < 0) st.first_ms = ms;
  st.last_ms = ms;

  if (status == BT::NodeStatus::RUNNING) {
    if (prev == BT::NodeStatus::IDLE) {
      st.attempts++;
    }
    st.run_start_ms = ms;
    st.in_running = true;
  } else if (status == BT::NodeStatus::SUCCESS ||
             status == BT::NodeStatus::FAILURE ||
             status == BT::NodeStatus::IDLE) {
    if (prev == BT::NodeStatus::IDLE) {
      // 同步节点:IDLE 直达终态,没有 RUNNING 迁移
      st.attempts++;
    }
    if (st.in_running) {
      st.running_ms += ms - st.run_start_ms;
      st.in_running = false;
    }
  }
}

nlohmann::json explainTreeNode(const BT::TreeNode* node,
                               const std::string& path, NodeStatsMap* stats) {
  nlohmann::json j;
  j["name"] = node->name();
  j["type"] = node->registrationName();
  j["status"] = std::string(BT::toStr(node->status()));
  const std::string here =
      path.empty() ? node->name() : path + "/" + node->name();
  j["path"] = here;

  if (stats) {
    std::lock_guard<std::mutex> lk(stats->mu);
    auto it = stats->by_path.find(node->fullPath());
    if (it != stats->by_path.end()) {
      const auto& st = it->second;
      j["attempts"] = st.attempts;
      j["running_ms"] = std::round(st.running_ms * 10.0) / 10.0;
      j["first_ms"] = std::round(st.first_ms * 10.0) / 10.0;
      j["last_ms"] = std::round(st.last_ms * 10.0) / 10.0;
    }
  }

  nlohmann::json children = nlohmann::json::array();
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      children.push_back(explainTreeNode(child, here, stats));
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      children.push_back(explainTreeNode(dec->child(), here, stats));
    }
  }
  j["children"] = children;
  return j;
}

} // namespace detail
} // namespace nebulabt
