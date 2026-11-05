#pragma once
/// 任务模型:TaskSpec(提交) / TaskHandle(查询、取消、explain)。

#include "nebulabt/core/failure.hpp"
#include "nebulabt/core/journal.hpp"
#include <behaviortree_cpp/contrib/json.hpp>
#include <chrono>
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
  /// 任务 watchdog(毫秒,M4):RUNNING 期间静默(无节点状态迁移)超过
  /// 该值触发 Engine::setWatchdog 的回调;> 0 时覆盖引擎默认值,
  /// 是否 halt 由引擎的 halt_on_trigger 决定。0 = 跟随引擎默认
  uint64_t watchdog_ms = 0;
  /// 断点恢复(M2,可选):上一轮任务的事件日志(TaskHandle::journal()
  /// 或 journal sink 落盘后的数组)。提交同一棵树时,已完成的 Checkpoint
  /// 跳过子节点并恢复黑板——上一轮已提交的副作用不重复执行。
  nlohmann::json resume_journal = nlohmann::json::array();
  /// 树包版本显式钉住(M3,可选):非空时路由到指定 pack 版本,
  /// 忽略灰度策略(恢复重跑必须与原版本一致,恢复路径内部使用)
  std::string pack_version;
  /// 资源仲裁(M5,可选):任务声明占用的互斥资源(如 "gripper","bin_3")。
  /// 与任一活动任务的声明集有交集 → 提交被拒绝(返回空句柄);
  /// 任务终态时自动释放。单进程内的任务级互斥,防"两个任务同时抓同一包裹"
  std::vector<std::string> resources;
  /// Groot2 实时可视化端口(M7,可选;0 = 关闭):该任务运行期间向
  /// Groot2 GUI 实时推送节点状态(BT.CPP 自带 publisher)。
  /// 每个运行中任务需独立端口;端口被占用时告警并继续(不影响执行)
  uint16_t groot2_port = 0;
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

  /// 带超时等待;超时返回当前状态(通常仍为 RUNNING)
  TaskState waitFor(std::chrono::milliseconds timeout) const;

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
