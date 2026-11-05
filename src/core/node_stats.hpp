#pragma once
/// 节点计时与 explain 树遍历(explain 完整版,M2)。
/// 每次 IDLE->RUNNING 计一次 attempt;RUNNING->终态 累计运行时长。

#include "nebulabt/core/clock.hpp"
#include "nebulabt/core/failure.hpp"
#include <behaviortree_cpp/contrib/json.hpp>
#include <behaviortree_cpp/loggers/abstract_logger.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace BT {
class TreeNode;
}

namespace nebulabt {
namespace detail {

struct NodeStat {
  int attempts = 0;
  double running_ms = 0;   // 累计 RUNNING 时长(相对任务起点)
  double first_ms = -1;    // 首次状态迁移
  double last_ms = -1;     // 最近状态迁移
  double run_start_ms = 0; // 本次 RUNNING 的起点
  bool in_running = false;
};

struct NodeStatsMap {
  std::mutex mu;
  std::map<std::string, NodeStat> by_path;
};

/// 任务活跃时间戳(watchdog 用):任一节点状态迁移即刷新
using ActivityStamp = std::atomic<int64_t>;  // steady_clock 毫秒

// 订阅全树状态迁移(BT.CPP StatusChangeLogger),填充 NodeStatsMap。
// 时间戳用相对模式(相对 logger 构造时刻 ≈ 任务提交时刻)。
class TaskTraceLogger : public BT::StatusChangeLogger {
public:
  /// activity:watchdog 活动戳(时钟毫秒);clock:与引擎一致的时钟
  TaskTraceLogger(BT::TreeNode* root, std::shared_ptr<NodeStatsMap> stats,
                  std::shared_ptr<ActivityStamp> activity = nullptr,
                  std::shared_ptr<NebulaClock> clock = nullptr);

  void callback(BT::Duration timestamp, const BT::TreeNode& node,
                BT::NodeStatus prev, BT::NodeStatus status) override;

  void flush() override {}

private:
  std::shared_ptr<NodeStatsMap> stats_;
  std::shared_ptr<ActivityStamp> activity_;
  std::shared_ptr<NebulaClock> clock_;
};

/// explain() 的树遍历:节点路径 + 当前状态 + 计时(attempts / running_ms)
nlohmann::json explainTreeNode(const BT::TreeNode* node,
                               const std::string& path, NodeStatsMap* stats);

} // namespace detail
} // namespace nebulabt
