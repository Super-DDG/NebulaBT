#pragma once
/// 任务模型:TaskSpec(提交) / TaskHandle(查询、取消、explain)。

#include "nebulabt/core/failure.hpp"
#include <behaviortree_cpp/contrib/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace nebulabt {

namespace detail {
struct TaskContext;
struct EngineCore;
}

enum class TaskState { Queued, Running, Succeeded, Failed, Canceled };
const char* toString(TaskState s);
inline bool isTerminal(TaskState s) {
  return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Canceled;
}

/// 任务提交描述
struct TaskSpec {
  std::string task_id;    // 留空则自动生成
  std::string task_type;  // 注入黑板 task_type
  /// slots 注入黑板规则:
  ///   string -> 黑板 string
  ///   array  -> 黑板 vector<string>(ForEach 直接可用)
  ///   其它   -> dump() 为 string
  nlohmann::json slots = nlohmann::json::object();
  /// 任务级超时(毫秒,0 = 不限制)。超时触发:halt 整棵树 +
  /// 记录 TASK_TIMEOUT 失败 + 终态 FAILED(策略引擎 M1 的第一块)
  uint64_t timeout_ms = 0;
};

/// 任务句柄
class TaskHandle {
public:
  TaskHandle() = default;

  explicit operator bool() const { return static_cast<bool>(ctx_); }
  bool empty() const { return !ctx_; }

  std::string taskId() const;
  TaskState state() const;

  /// 阻塞等待终态并返回(测试/示例用;服务侧请用 Engine::onFinished 回调)
  TaskState wait();

  /// 请求取消:halt 整棵树(回调各动作 halt())后进入 Canceled
  bool cancel();

  /// 结构化失败快照
  std::vector<FailureRecord> failures() const;

  /// 执行报告:树形结构 + 各节点状态 + 失败记录
  nlohmann::json explain() const;

private:
  friend class Engine;
  friend struct detail::EngineCore;
  TaskHandle(std::shared_ptr<detail::TaskContext> ctx,
             std::shared_ptr<detail::EngineCore> core);

  std::shared_ptr<detail::TaskContext> ctx_;
  std::shared_ptr<detail::EngineCore> core_;
};

} // namespace nebulabt
