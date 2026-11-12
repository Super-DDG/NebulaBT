#pragma once
/// 任务运行时上下文(engine 内部)。

#include "nebulabt/core/failure.hpp"
#include "nebulabt/core/journal.hpp"
#include "nebulabt/core/task.hpp"
#include "node_stats.hpp"
#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/contrib/json.hpp>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace BT {
class Tree;
class StatusChangeLogger;
#ifndef NEBULABT_NO_GROOT2
class Groot2Publisher;
#endif
}

namespace nebulabt {
namespace detail {

struct TaskContext {
  std::string task_id;
  std::string task_type;
  std::string pack_name;    // 来源树包(M3;直接提交 XML 时为空)
  std::string pack_version;
  std::shared_ptr<BT::Tree> tree;
  std::shared_ptr<NodeStatsMap> node_stats = std::make_shared<NodeStatsMap>();
  std::shared_ptr<ActivityStamp> activity =
      std::make_shared<ActivityStamp>(0);  // 最近节点迁移时刻(watchdog)
  std::shared_ptr<BT::StatusChangeLogger> trace;  // 声明在 tree 之后:先于树析构
#ifndef NEBULABT_NO_GROOT2
  std::shared_ptr<BT::Groot2Publisher> groot2;    // M7:实时可视化(可选)
#endif
  std::shared_ptr<FailureLog> failures = std::make_shared<FailureLog>();
  std::atomic<TaskState> state{TaskState::Queued};
  std::atomic<bool> finished{false};
  std::atomic<bool> watchdog_fired{false};  // 本轮静默期已触发过(M4)
  std::atomic<bool> journal_capped{false};  // journal 触顶已告警(只报一次)
  std::mutex wait_mu;
  std::condition_variable wait_cv;

  // journal(M2):任务事件日志 + 已完成检查点的黑板快照
  // 内存上限由 EngineLimits.max_journal_events 控制(默认 1000)
  std::mutex journal_mu;
  std::vector<JournalEvent> journal;
  // 仅含从 resume_journal 种子的检查点(上一轮的完成历史)。
  // 本 run 新完成的检查点只进 journal,不进此 map——否则循环体内
  // 第二次迭代会被 tryResume 误判"已完成"而跳过工作。
  std::map<std::string, nlohmann::json> resume_checkpoints;
};

} // namespace detail
} // namespace nebulabt
