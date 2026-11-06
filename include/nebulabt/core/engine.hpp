#pragma once
/// NebulaBT 引擎:任务生命周期 + 事件驱动调度 + 结构化失败。
/// 分层与子系统职责见 docs/DESIGN.md。
///
/// 用法:
///   Engine engine;
///   engine.registerAction("MyNode", std::make_shared<MyAction>());
///   auto task = engine.submitFromText(xml, spec);
///   task.wait();
///   task.explain();

#include "nebulabt/core/clock.hpp"
#include "nebulabt/core/pack.hpp"
#include "nebulabt/core/task.hpp"
#include "nebulabt/sdk/action.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace BT {
class Tree; // 前向声明(全局 ::BT),仅用于 Engine 私有成员的 shared_ptr
}

namespace nebulabt {

enum class LogLevel { Info, Warn, Error };

/// 引擎资源上限(M4):0 = 不限。嵌入式/长期运行服务建议显式设置,
/// 防止失控的任务/回调无界增长吃掉内存
struct EngineLimits {
  /// 同时运行(含排队)的最大任务数;超限提交被拒绝并返回空句柄
  size_t max_active_tasks = 0;
  /// scheduler 作业队列上限;超限投递被拒绝并告警
  size_t max_pending_jobs = 0;
  /// 未触发定时器上限;超限投递被拒绝并告警
  size_t max_pending_timers = 0;
  /// 单任务内存 journal 事件上限(0 = 不限)。触顶:新事件不再入内存
  /// (sink 落盘不受影响),并告警一次——上限太小会丢检查点事件,
  /// 恢复时已完成段被重执行,按任务事件量调大
  size_t max_journal_events = 1000;
};

class Engine {
public:
  /// 任务终态回调(在调度线程执行,应当快速返回)
  using FinishedCallback = std::function<void(TaskHandle)>;
  using LogCallback = std::function<void(LogLevel, const std::string&)>;
  /// journal 事件回调(调度线程执行;持久化即在此落盘,每事件一行 JSON)
  using JournalSink = std::function<void(const JournalEvent&)>;
  /// watchdog 触发回调(调度线程执行):task_id + 无节点迁移的静默毫秒数
  using WatchdogCallback =
      std::function<void(const std::string& task_id, uint64_t quiet_ms)>;

  /// vclock 非空 = 虚拟时钟模式(M6):超时/重试延时/watchdog 均随
  /// advanceTime() 推进而触发,测试零真实等待。默认真实时钟。
  explicit Engine(std::shared_ptr<VirtualClock> vclock = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// 注册业务动作;registrationId 即树 XML 中的节点标签。
  /// 端口声明取自 action 实例(运行期),经 BT.CPP 的
  /// registerNodeType(id, ports, args...) 显式注册。
  void registerAction(const std::string& registrationId, NebulaActionPtr action);

  TaskHandle submitFromText(const std::string& xmlText, TaskSpec spec = {});
  TaskHandle submitFromFile(const std::string& xmlPath, TaskSpec spec = {});

  /// 加载树包(目录须含 pack.json)。加载即全量校验:每棵树可解析、
  /// schema 数据流检查通过,失败抛异常——CI 中加载全部 pack 即门禁。
  /// 注意:先 registerAction 再 loadPack(树中的节点必须已注册)。
  /// manifestDir 指向含 pack.json(或 pack.yaml,YAML 支持编译启用时)
  /// 的目录;加载即校验全部树,失败抛异常(CI 门禁)。
  void loadPack(const std::string& manifestDir);

  /// 已加载 pack 提供的任务类型清单(编排宿主展示/路由用)
  struct TaskTypeInfo {
    std::string task_type;
    std::string pack;
    std::string version;
  };
  std::vector<TaskTypeInfo> loadedTaskTypes() const;

  /// 按 task_type 从已加载树包解析树文件并提交(spec.schema/timeout_ms
  /// 未显式给出时,采用 pack 声明的默认值)
  TaskHandle submitTask(TaskSpec spec);

  /// 灰度路由:task_id 哈希 % 100 < percent 的任务路由到指定版本,
  /// 其余走该 pack 的最高版本;TaskSpec.pack_version 显式钉住优先
  void setCanary(const std::string& packName, const std::string& version,
                 int percent);

  /// 加载期校验(不提交任务):解析树 + 按 schema 做 L2 树↔schema
  /// 数据流检查。返回错误列表(空 = 通过)。`nebula validate` CLI 的后端。
  std::vector<std::string> validate(const std::string& xmlText,
                                    const nlohmann::json& schema = {});

  void onFinished(FinishedCallback cb);

  /// journal 事件回调(提交/启动/检查点/终态;持久化即在此写文件/DB)
  void setJournalSink(JournalSink cb);

  /// 日志回调(默认输出到 stderr)
  void setLogger(LogCallback cb);

  /// 资源上限(M4)。可在任意时刻调用;建议引擎创建后立即设置
  void setLimits(const EngineLimits& limits);

  /// 仅虚拟时钟模式:推进虚拟时间 ms,到期定时器(超时/重试延时/
  /// watchdog 巡检)随即在调度线程触发。真实时钟模式无效并告警。
  void advanceTime(uint64_t ms);

  /// 失败码登记表(M8):失败码是接口——先声明后使用。
  /// 声明后,ctx.fail() 使用未登记的码会在 FailureRecord 标记
  /// undeclared_code(explain 可筛出拼写错误);登记表为空 = 不校验。
  /// pack 清单的 task.failure_codes 在 loadPack 时自动声明。
  void declareFailureCodes(const std::map<std::string, std::string>& codes);

  /// 任务 watchdog(M4):任务 RUNNING 期间超过 interval 毫秒无任何节点
  /// 状态迁移 → 触发回调并告警;halt_on_trigger 时同时 halt 整棵树、
  /// 记录 TASK_WATCHDOG 失败并终态 FAILED。
  /// interval = 0 关闭(默认)。TaskSpec.watchdog_ms > 0 可按任务覆盖。
  /// 嵌入式防"任务悄悄卡死":崩溃可恢复,卡死必须可发现。
  void setWatchdog(std::chrono::milliseconds interval, bool halt_on_trigger,
                   WatchdogCallback cb = {});

private:
  TaskHandle submitTree(std::shared_ptr<BT::Tree> tree, TaskSpec spec,
                        const std::string& packName = {},
                        const std::string& packVersion = {});
  TaskHandle submitFromFile(const std::string& xmlPath, TaskSpec spec,
                            const std::string& packName,
                            const std::string& packVersion);
  bool validateTask(BT::Tree& tree, const TaskSpec& spec) const;

  std::shared_ptr<detail::EngineCore> core_;
};

} // namespace nebulabt
