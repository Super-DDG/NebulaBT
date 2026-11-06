#include "engine_core.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <cstdio>

namespace nebulabt {
namespace detail {

EngineCore::~EngineCore() = default;

void EngineCore::logf(LogLevel lv, const std::string& msg) {
  Engine::LogCallback cb;
  {
    std::lock_guard<std::mutex> lk(mu);
    cb = log;
  }
  // 锁外回调(与 journal_sink/on_finished 同款):logger 内重入
  // Engine API(setLogger/submitTask/…)不死锁
  if (cb) cb(lv, msg);
}

// ---- journal ----

void EngineCore::appendJournal(const std::shared_ptr<TaskContext>& ctx,
                               const std::string& event, nlohmann::json data) {
  JournalEvent e;
  e.timestamp = nowTimestamp();
  e.task_id = ctx->task_id;
  e.task_type = ctx->task_type;
  e.event = event;
  e.data = std::move(data);
  Engine::JournalSink sink;
  size_t cap = 0;
  {
    std::lock_guard<std::mutex> lk(mu);
    sink = journal_sink;
    cap = limits.max_journal_events;
  }
  bool capped = false;
  {
    std::lock_guard<std::mutex> lk(ctx->journal_mu);
    if (cap == 0 || ctx->journal.size() < cap) {
      ctx->journal.push_back(e);
    } else {
      capped = true;  // 内存上限:sink 落盘不受影响,丢的只是 explain 视图
    }
  }
  if (capped && !ctx->journal_capped.exchange(true)) {
    // 触顶必须可发现:静默截断会丢检查点事件,恢复时已完成段被重执行
    logf(LogLevel::Warn,
         "task " + ctx->task_id + " in-memory journal capped at " +
             std::to_string(cap) +
             " events (sink still receives all; raise "
             "EngineLimits.max_journal_events if resume matters)");
  }
  if (sink) {
    sink(e);  // 锁外回调,避免死锁
  }
}

// ---- ICheckpointHost ----

bool EngineCore::checkpointTryResume(const std::string& task_id,
                                     const std::string& cp_id,
                                     nlohmann::json* out_snapshot) {
  auto ctx = findTask(task_id);
  if (!ctx) return false;
  bool found = false;
  nlohmann::json snap;
  {
    std::lock_guard<std::mutex> lk(ctx->journal_mu);
    auto it = ctx->resume_checkpoints.find(cp_id);
    if (it != ctx->resume_checkpoints.end()) {
      snap = it->second;
      found = true;
    }
  }
  if (found) {
    // 锁外记录(appendJournal 会再锁 journal_mu,不可重入)。
    // 同时补一条 "checkpoint" 事件:多跳恢复(run3 从 run2 的 journal
    // 恢复)时,完成历史得以延续,不会因只有 restored 事件而断链。
    appendJournal(ctx, "checkpoint",
                  nlohmann::json{{"id", cp_id}, {"blackboard", snap}});
    appendJournal(ctx, "checkpoint_restored", nlohmann::json{{"id", cp_id}});
  }
  return found;
}

void EngineCore::checkpointRecord(const std::string& task_id,
                                  const std::string& cp_id,
                                  const nlohmann::json& snapshot) {
  auto ctx = findTask(task_id);
  if (!ctx) return;
  // 只进 journal:本 run 的完成历史;resume 历史只在提交时种子
  appendJournal(ctx, "checkpoint",
                nlohmann::json{{"id", cp_id}, {"blackboard", snapshot}});
}

std::shared_ptr<TaskContext> EngineCore::findTask(const std::string& id) {
  std::lock_guard<std::mutex> lk(mu);
  auto it = active.find(id);
  return it != active.end() ? it->second : nullptr;
}

std::shared_ptr<FailureLog> EngineCore::findFailures(const std::string& taskId) {
  auto ctx = findTask(taskId);
  return ctx ? ctx->failures : nullptr;
}

void EngineCore::tickTask(const std::string& id) {
  auto ctx = findTask(id);
  if (!ctx) return;

  TaskState expected = TaskState::Queued;
  if (ctx->state.compare_exchange_strong(expected, TaskState::Running)) {
    appendJournal(ctx, "started", nlohmann::json::object());
  }
  if (ctx->state.load() != TaskState::Running) {
    return; // 已终态(如已取消)
  }

  BT::NodeStatus status;
  try {
    status = ctx->tree->tickOnce();
  } catch (const std::exception& e) {
    // 结构化记录(此前只进日志):节点构造/校验抛错(如 Compensable
    // 子树数不符)也要可被 explain 检索
    FailureRecord fr;
    fr.code = "TICK_EXCEPTION";
    fr.node = id;
    fr.node_type = "Engine";
    fr.message = e.what();
    fr.timestamp = nowTimestamp();
    ctx->failures->add(std::move(fr));
    logf(LogLevel::Error, "task " + id + " tick exception: " + e.what());
    finishTask(ctx, TaskState::Failed, e.what());
    return;
  }

  if (status == BT::NodeStatus::RUNNING) return;
  finishTask(ctx,
             status == BT::NodeStatus::SUCCESS ? TaskState::Succeeded
                                               : TaskState::Failed,
             std::string("tree returned ") + BT::toStr(status));
}

void EngineCore::finishTask(std::shared_ptr<TaskContext> ctx, TaskState st,
                            const std::string& reason) {
  if (ctx->finished.exchange(true)) {
    return; // 只允许一次终态迁移
  }
  ctx->state.store(st);

  // M5:Saga 补偿必须在唤醒 wait() 之前完成——调用方从 wait() 返回时,
  // 补偿副作用与 COMPENSATION_FAILED 记录必须已经落地。
  // 失败终态 → 逆序执行已武装补偿;其余终态丢弃补偿栈
  if (st == TaskState::Failed) {
    runCompensations(ctx);
  } else {
    sagaClear(ctx->task_id);
  }

  {
    std::lock_guard<std::mutex> lk(mu);
    active.erase(ctx->task_id);
    task_resources.erase(ctx->task_id);  // M5:释放资源
  }

  // finished 事件必须在唤醒 wait() **之前**落 journal:调用方从 wait()
  // 返回即可能持久化 journal(恢复依据),迟到的终态事件会丢
  // (ASan 慢时序下实证的竞态)
  appendJournal(ctx, "finished",
                nlohmann::json{{"state", toString(st)}, {"reason", reason}});

  // 终端日志在唤醒 wait() 之前发出:调用方观察到终态时,日志回调已
  // 执行(与 finished 事件同理;A2 回归测试曾捕获此竞态)
  logf(LogLevel::Info, "task " + ctx->task_id + " " + toString(st) +
                           (reason.empty() ? "" : " (" + reason + ")"));

  {
    // 与 wait() 的谓词检查建立顺序,防止丢唤醒
    std::lock_guard<std::mutex> lk(ctx->wait_mu);
  }
  ctx->wait_cv.notify_all();

  Engine::FinishedCallback cb;
  {
    std::lock_guard<std::mutex> lk(mu);
    cb = on_finished;
  }
  if (cb) {
    cb(TaskHandle(ctx, shared_from_this()));
  }
}

// ---- ISagaHost(M5):补偿栈 ----

void EngineCore::sagaPush(const std::string& task_id, CompensableNode* node,
                          const std::string& saga_id) {
  {
    std::lock_guard<std::mutex> lk(mu);
    saga_stacks[task_id].push_back({node, saga_id});
  }
  if (auto ctx = findTask(task_id)) {
    // findTask/sagaPush 均在调度线程(tick 路径)调用,无重入风险
    appendJournal(ctx, "compensation_armed", nlohmann::json{{"id", saga_id}});
  }
}

std::vector<std::pair<CompensableNode*, std::string>> EngineCore::sagaPopAll(
    const std::string& task_id) {
  std::lock_guard<std::mutex> lk(mu);
  auto it = saga_stacks.find(task_id);
  if (it == saga_stacks.end()) {
    return {};
  }
  auto out = std::move(it->second);
  saga_stacks.erase(it);
  return out;
}

void EngineCore::sagaClear(const std::string& task_id) {
  std::lock_guard<std::mutex> lk(mu);
  saga_stacks.erase(task_id);
}

void EngineCore::runCompensations(const std::shared_ptr<TaskContext>& ctx) {
  // 武装顺序取出,逆序执行(后完成的先补偿)
  const auto entries = sagaPopAll(ctx->task_id);
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    const std::string& id = it->second;
    appendJournal(ctx, "compensation_started", nlohmann::json{{"id", id}});
    BT::NodeStatus st = BT::NodeStatus::FAILURE;
    std::string err;
    try {
      st = it->first->executeCompensation();
    } catch (const std::exception& e) {
      err = e.what();
    }
    appendJournal(ctx, "compensation_done",
                  nlohmann::json{{"id", id},
                                 {"status", std::string(BT::toStr(st))}});
    if (err.empty() && st == BT::NodeStatus::SUCCESS) {
      continue;
    }
    FailureRecord fr;
    fr.code = "COMPENSATION_FAILED";
    fr.node = id;
    fr.node_type = "Compensable";
    fr.message = err.empty()
                     ? "compensation returned " + std::string(BT::toStr(st))
                     : "compensation exception: " + err;
    fr.timestamp = nowTimestamp();
    ctx->failures->add(std::move(fr));
    logf(LogLevel::Warn, "task " + ctx->task_id +
                             " compensation '" + id + "' failed");
  }
}

// ---- IGuardHost(M5)----

void EngineCore::guardViolated(const std::string& task_id,
                               const std::string& node_name,
                               const std::string& guard_key) {
  auto ctx = findTask(task_id);
  if (!ctx) {
    return;
  }
  FailureRecord fr;
  fr.code = "GUARD_VIOLATED";
  fr.node = node_name;
  fr.node_type = "ReactiveGuard";
  fr.message = "guard key '" + guard_key + "' is false or missing";
  fr.timestamp = nowTimestamp();
  ctx->failures->add(std::move(fr));
  appendJournal(ctx, "guard_violated",
                nlohmann::json{{"node", node_name}, {"key", guard_key}});
}

// ---- watchdog(M4):卡死检测 ----
// 巡检定时器周期 = interval;触发条件 = 距最近一次节点状态迁移
// (activity)已静默 >= interval。触发一次后置位 watchdog_fired,
// 直到活动恢复(< interval)重新武装,避免同一静默期反复触发。

void EngineCore::armWatchdog(std::shared_ptr<TaskContext> ctx,
                             uint64_t interval) {
  auto core = shared_from_this();
  const auto period = std::chrono::milliseconds(interval);
  scheduler.callLater(period, [core, ctx, interval] {
    if (ctx->finished.load()) {
      return;  // 任务已终态:停止巡检
    }
    const int64_t now = static_cast<int64_t>(core->clock->nowMs());
    const int64_t quiet = now - ctx->activity->load();
    bool halt = false;
    if (quiet >= static_cast<int64_t>(interval)) {
      bool expected = false;
      if (ctx->watchdog_fired.compare_exchange_strong(expected, true)) {
        Engine::WatchdogCallback cb;
        {
          std::lock_guard<std::mutex> lk(core->mu);
          cb = core->watchdog_cb;
          halt = core->watchdog_halt;
        }
        if (cb) {
          cb(ctx->task_id, static_cast<uint64_t>(quiet));
        }
        core->logf(LogLevel::Warn,
                   "task " + ctx->task_id + " watchdog: no node transition for " +
                       std::to_string(quiet) + " ms");
        if (halt) {
          FailureRecord fr;
          fr.code = "TASK_WATCHDOG";
          fr.node = "engine";
          fr.node_type = "Engine";
          fr.message = "no node transition for " + std::to_string(quiet) + " ms";
          fr.timestamp = nowTimestamp();
          ctx->failures->add(std::move(fr));
          ctx->tree->haltTree();
          core->finishTask(ctx, TaskState::Failed, "task watchdog");
          return;
        }
      }
    } else {
      ctx->watchdog_fired.store(false);  // 活动恢复:重新武装
    }
    core->scheduler.callLater(std::chrono::milliseconds(interval),
                              [core, ctx, interval] {
                                core->armWatchdog(ctx, interval);
                              });
  });
}

// ---- 树包路由(M3)----

std::array<int, 3> EngineCore::parseVersion(const std::string& v) {
  std::array<int, 3> out{0, 0, 0};
  std::sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
  return out;
}

bool EngineCore::versionLess(const std::string& a, const std::string& b) {
  return parseVersion(a) < parseVersion(b);
}

uint64_t EngineCore::hashId(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (const char c : s) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    h *= 1099511628211ull;
  }
  return h;
}

const EngineCore::PackEntry* EngineCore::resolveTask(
    const std::string& taskType, const std::string& explicitVersion,
    const std::string& taskId) {
  std::lock_guard<std::mutex> lk(mu);
  const PackEntry* best = nullptr;
  const PackEntry* canaryHit = nullptr;
  const PackEntry* pinned = nullptr;
  for (const auto& p : packs) {
    if (!p.manifest.tasks.count(taskType)) continue;
    if (!best || versionLess(best->manifest.version, p.manifest.version)) {
      best = &p;
    }
    if (canary.percent > 0 && canary.pack == p.manifest.name &&
        canary.version == p.manifest.version &&
        (hashId(taskId) % 100) < static_cast<uint64_t>(canary.percent)) {
      canaryHit = &p;
    }
    if (!explicitVersion.empty() && p.manifest.version == explicitVersion) {
      pinned = &p;
    }
  }
  if (pinned) return pinned;
  if (canaryHit) return canaryHit;
  return best;
}

} // namespace detail
} // namespace nebulabt
