#pragma once
/// 任务模型:TaskSpec(提交) / TaskHandle(查询、取消、explain)。

#include "nebulabt/core/failure.hpp"
#include "nebulabt/core/journal.hpp"
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
  ///   schema 声明为 int/double/bool 且类型匹配 -> 按真实类型注入
  ///   string -> 黑板 string
  ///   array  -> 黑板 vector<string>(ForEach 直接可用)
  ///   其它   -> dump() 为 string
  nlohmann::json slots = nlohmann::json::object();
  /// 任务 slots schema(可选;格式见 core/schema.hpp)。
  /// 提供后启用加载期严格校验:L1 提交↔schema + L2 树↔schema 数据流检查;
  /// 不提供 = 关闭校验,完全向后兼容。
  nlohmann::json schema = nlohmann::json::object();
  /// 任务级超时(毫秒,0 = 不限制)。超时触发:halt 整棵树 +
  /// 记录 TASK_TIMEOUT 失败 + 终态 FAILED(策略引擎 M1 的第一块)
  uint64_t timeout_ms = 0;
  /// 断点恢复(M2,可选):上一轮任务的事件日志(TaskHandle::journal()
  /// 或 journal sink 落盘后的数组)。提交同一棵树时,已完成的 Checkpoint
  /// 跳过子节点并恢复黑板——上一轮已提交的副作用不重复执行。
  nlohmann::json resume_journal = nlohmann::json::array();
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

  /// 任务事件日志快照(可整体作为新任务的 resume_journal)
  std::vector<JournalEvent> journal() const;

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
