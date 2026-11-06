#pragma once
/// 引擎核心:调度、任务生命周期、journal、检查点宿主、树包路由。

#include "nebulabt/core/engine.hpp"
#include "nebulabt/core/clock.hpp"
#include "nebulabt/core/scheduler.hpp"
#include "task_context.hpp"
#include "nebulabt/nodes/checkpoint.hpp"
#include "nebulabt/nodes/compensable.hpp"
#include "nebulabt/nodes/reactive_guard.hpp"
#include <behaviortree_cpp/bt_factory.h>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nebulabt {
namespace detail {

struct EngineCore : std::enable_shared_from_this<EngineCore>, ICheckpointHost,
                    ISagaHost, IGuardHost {
  // 外联析构(诊断见 engine_core.cpp):隐式内联析构 + 本类成员布局
  // 下,销毁路径曾被编译器折叠,部分注册表存储未被释放(ASan 实证)
  ~EngineCore();
  // ---- 树包注册表(M3)----
  struct PackEntry {
    PackManifest manifest;
    std::string dir;  // manifest 所在目录
  };
  struct Canary {
    std::string pack;
    std::string version;
    int percent = 0;
  };

  BT::BehaviorTreeFactory factory;
  Scheduler scheduler;
  std::shared_ptr<NebulaClock> clock;  // 真实/虚拟时钟(M6);watchdog/活动戳用
  std::mutex mu;
  // 树创建串行化:BT.CPP factory 内部持有共享 XMLParser(bt_factory.cpp
  // 的 _p->parser),并发 createTreeFromText/FromFile 会竞争它——库未
  // 插桩,TSan/ASan 不可见;并发压测实证(Ok 任务偶发 FAILED)
  std::mutex create_mu;
  std::unordered_map<std::string, std::shared_ptr<TaskContext>> active;
  std::vector<PackEntry> packs;  // mu 保护
  Canary canary;                 // mu 保护
  Engine::FinishedCallback on_finished;  // mu 保护
  Engine::JournalSink journal_sink;      // mu 保护
  Engine::LogCallback log = [](LogLevel, const std::string& msg) {
    std::fprintf(stderr, "[nebulabt] %s\n", msg.c_str());
  };

  // ---- watchdog(M4):任务卡死检测 ----
  uint64_t watchdog_ms = 0;     // mu 保护;0 = off
  bool watchdog_halt = false;   // mu 保护
  Engine::WatchdogCallback watchdog_cb;  // mu 保护

  // ---- 资源上限(M4)----
  EngineLimits limits;          // mu 保护

  // ---- Saga 补偿栈(M5)----
  std::unordered_map<std::string,
                     std::vector<std::pair<CompensableNode*, std::string>>>
      saga_stacks;  // mu 保护

  // ---- 资源仲裁(M5)----
  std::unordered_map<std::string, std::vector<std::string>> task_resources;
  // mu 保护;task_id -> 声明的资源

  // ---- 失败码登记表(M8)----
  std::shared_ptr<FailureCodeTable> failure_codes =
      std::make_shared<FailureCodeTable>();

  /// 启动任务的 watchdog 巡检定时器(interval>0 时)
  void armWatchdog(std::shared_ptr<TaskContext> ctx, uint64_t interval);

  void logf(LogLevel lv, const std::string& msg);

  // ---- journal ----
  void appendJournal(const std::shared_ptr<TaskContext>& ctx,
                     const std::string& event, nlohmann::json data);

  // ---- ICheckpointHost ----
  bool checkpointTryResume(const std::string& task_id,
                           const std::string& cp_id,
                           nlohmann::json* out_snapshot) override;
  void checkpointRecord(const std::string& task_id, const std::string& cp_id,
                        const nlohmann::json& snapshot) override;

  // ---- ISagaHost(M5:补偿栈,mu 保护)----
  void sagaPush(const std::string& task_id, CompensableNode* node,
                const std::string& saga_id) override;
  std::vector<std::pair<CompensableNode*, std::string>> sagaPopAll(
      const std::string& task_id) override;
  void sagaClear(const std::string& task_id) override;

  /// 任务 FAILED 终态前:逆序执行已武装补偿;失败记 COMPENSATION_FAILED
  /// 并继续(不中断其余补偿)。须在调度线程调用(树 tick 线程模型)。
  void runCompensations(const std::shared_ptr<TaskContext>& ctx);

  // ---- IGuardHost(M5)----
  void guardViolated(const std::string& task_id, const std::string& node_name,
                     const std::string& guard_key) override;

  std::shared_ptr<TaskContext> findTask(const std::string& id);
  std::shared_ptr<FailureLog> findFailures(const std::string& taskId);
  void tickTask(const std::string& id);
  void finishTask(std::shared_ptr<TaskContext> ctx, TaskState st,
                  const std::string& reason);

  // ---- 树包路由(M3)----
  static std::array<int, 3> parseVersion(const std::string& v);
  static bool versionLess(const std::string& a, const std::string& b);
  static uint64_t hashId(const std::string& s);  // FNV-1a:灰度确定性哈希
  // 按 task_type 选 pack 条目:显式版本 > 灰度 > 最高版本
  const PackEntry* resolveTask(const std::string& taskType,
                               const std::string& explicitVersion,
                               const std::string& taskId);
};

} // namespace detail
} // namespace nebulabt
