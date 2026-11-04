#pragma once
/// Journal:任务生命周期事件日志(M2)。
///
/// 一鱼两吃:既是 explain/审计的观测数据,也是断点恢复的恢复依据。
/// 每个事件一行 JSON(经 sink 落盘后即可跨进程恢复)。
///
/// 事件类型:
///   submitted          任务提交(data: task_type)
///   started            首次 tick
///   checkpoint         Checkpoint 完成(data: id, blackboard 快照)
///   checkpoint_restored 恢复时命中已完成的 Checkpoint(data: id)
///   finished           终态(data: state, reason)

#include <behaviortree_cpp/contrib/json.hpp>
#include <string>

namespace nebulabt {

struct JournalEvent {
  std::string timestamp;
  std::string task_id;
  std::string task_type;
  std::string event;
  nlohmann::json data = nlohmann::json::object();
};

void to_json(nlohmann::json& j, const JournalEvent& e);
void from_json(const nlohmann::json& j, JournalEvent& e);

} // namespace nebulabt
