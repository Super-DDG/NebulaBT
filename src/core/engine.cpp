#include "nebulabt/core/engine.hpp"

#include "bridge_node.hpp"
#include "engine_core.hpp"
#include "node_stats.hpp"
#include "tree_check.hpp"
#include "nebulabt/core/schema.hpp"
#include "nebulabt/nodes/checkpoint.hpp"
#include "nebulabt/nodes/compensable.hpp"
#include "nebulabt/nodes/for_each.hpp"
#include "nebulabt/nodes/reactive_guard.hpp"
#include "nebulabt/nodes/retry_with_delay.hpp"
#include "nebulabt/nodes/switch_n.hpp"
#include "nebulabt/nodes/while_loop.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/behavior_tree.h>
#include <behaviortree_cpp/loggers/groot2_publisher.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace nebulabt {

namespace detail {

std::string generateTaskId() {
  static std::atomic<uint64_t> counter{0};
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return "task-" + std::to_string(ms) + "-" +
         std::to_string(counter.fetch_add(1));
}

} // namespace detail

// ============================================================================
// Engine
// ============================================================================

Engine::Engine(std::shared_ptr<VirtualClock> vclock)
  : core_(std::make_shared<detail::EngineCore>()) {
  // 时钟(M6):虚拟或真实;watchdog/活动戳与调度器定时器必须同源
  if (vclock) {
    core_->clock = vclock;
    core_->scheduler.setVirtualClock(vclock);
  } else {
    core_->clock = std::make_shared<SteadyClock>();
  }

  // 内置动态树原语与策略节点
  core_->factory.registerNodeType<ForEachNode>("ForEach");
  core_->factory.registerNodeType<WhileLoopNode>("WhileLoop");
  core_->factory.registerNodeType<SwitchNNode>("SwitchN");

  // RetryWithDelay 需要延时唤醒:注入调度器与任务唤醒函数。
  // 必须用 weak_ptr:factory 是 EngineCore 的成员,捕获 shared_ptr 会形成
  // EngineCore→factory→lambda→EngineCore 引用环,每个引擎实例整体泄漏
  // (ASan 泄漏报告抓到的真问题)。树创建时引擎必然存活,lock 失败即 no-op
  auto weak = std::weak_ptr<detail::EngineCore>(core_);
  core_->factory.registerNodeType<RetryWithDelayNode>(
      "RetryWithDelay", &core_->scheduler,
      [weak](const std::string& taskId) {
        if (auto core = weak.lock()) {
          core->scheduler.post([core, taskId] { core->tickTask(taskId); });
        }
      });

  // Checkpoint 需要读写任务的检查点历史与 journal:注入宿主(EngineCore)
  core_->factory.registerNodeType<CheckpointNode>("Checkpoint",
                                                  core_.get());

  // M5:Compensable 补偿原语(宿主管理任务的补偿栈)
  core_->factory.registerNodeType<CompensableNode>("Compensable",
                                                   core_.get());

  // M5:ReactiveGuard 运行期守护(定时巡检经调度器,宿主记录违规)
  // wake 同样用 weak_ptr 防 factory 引用环(见上)
  core_->factory.registerNodeType<ReactiveGuardNode>(
      "ReactiveGuard", &core_->scheduler,
      [weak](const std::string& taskId) {
        if (auto core = weak.lock()) {
          core->scheduler.post([core, taskId] { core->tickTask(taskId); });
        }
      },
      core_.get());

  core_->scheduler.start();
}

Engine::~Engine() {
  // 取消全部活动任务(halt + CANCELED),再等调度器把取消作业执行完——
  // 持有句柄的 wait() 会正常返回,不再永久挂起
  std::vector<std::shared_ptr<detail::TaskContext>> toCancel;
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (auto& [id, ctx] : core_->active) {
      toCancel.push_back(ctx);
    }
  }
  for (auto& ctx : toCancel) {
    core_->scheduler.post([core = core_, ctx] {
      if (ctx->finished.load()) {
        return;
      }
      ctx->tree->haltTree();
      core->finishTask(ctx, TaskState::Canceled, "engine destroyed");
    });
  }
  core_->scheduler.drainAndStop();
}

void Engine::registerAction(const std::string& registrationId,
                            NebulaActionPtr action) {
  if (!action) {
    throw std::invalid_argument("Engine::registerAction: null action");
  }
  // 端口来自 action 实例,BT.CPP 支持注册期显式传入 PortsList
  core_->factory.registerNodeType<detail::BridgeNode>(
      registrationId, action->providedPorts(), action, &core_->scheduler,
      core_.get());
}

TaskHandle Engine::submitFromText(const std::string& xmlText, TaskSpec spec) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  std::shared_ptr<BT::Tree> tree;
  try {
    std::lock_guard<std::mutex> lk(core_->create_mu);  // BT.CPP 共享解析器非并发安全
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromText(xmlText));
  } catch (const std::exception& e) {
    core_->logf(LogLevel::Error, "create tree failed: " + std::string(e.what()));
    return TaskHandle();
  }
  if (!validateTask(*tree, spec)) {
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec));
}

TaskHandle Engine::submitFromFile(const std::string& xmlPath, TaskSpec spec) {
  return submitFromFile(xmlPath, std::move(spec), "", "");
}

TaskHandle Engine::submitFromFile(const std::string& xmlPath, TaskSpec spec,
                                  const std::string& packName,
                                  const std::string& packVersion) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  std::shared_ptr<BT::Tree> tree;
  try {
    std::lock_guard<std::mutex> lk(core_->create_mu);  // BT.CPP 共享解析器非并发安全
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromFile(xmlPath));
  } catch (const std::exception& e) {
    core_->logf(LogLevel::Error, "create tree failed: " + std::string(e.what()));
    return TaskHandle();
  }
  if (!validateTask(*tree, spec)) {
    return TaskHandle();
  }
  return submitTree(std::move(tree), std::move(spec), packName, packVersion);
}

void Engine::loadPack(const std::string& manifestDir) {
  const std::string manifestPath = manifestDir + "/pack.json";
  std::ifstream f(manifestPath);
  if (!f) {
    throw std::runtime_error("loadPack: cannot read " + manifestPath);
  }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    throw std::runtime_error("loadPack: manifest parse: " +
                             std::string(e.what()));
  }
  auto manifest = parsePackManifest(j);

  // requires:依赖 pack 必须已加载且版本达标(拆包有依赖顺序保障)
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (const auto& req : manifest.requires) {
      bool satisfied = false;
      for (const auto& p : core_->packs) {
        if (p.manifest.name != req.pack) continue;
        if (req.min_version.empty() ||
            !detail::EngineCore::versionLess(p.manifest.version,
                                             req.min_version)) {
          satisfied = true;
          break;
        }
      }
      if (!satisfied) {
        throw std::runtime_error(
            "loadPack[" + manifest.name + "]: requires pack '" + req.pack +
            "' >= " + (req.min_version.empty() ? "any" : req.min_version) +
            " (load dependencies first)");
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(core_->mu);
    for (const auto& p : core_->packs) {
      if (p.manifest.name == manifest.name &&
          p.manifest.version == manifest.version) {
        throw std::runtime_error("loadPack: pack '" + manifest.name + "' v" +
                                 manifest.version + " already loaded");
      }
    }
  }

  // 加载即校验:每棵树可解析、schema 数据流通过——CI 加载全部 pack 即门禁
  for (const auto& [taskType, task] : manifest.tasks) {
    const std::string path = manifestDir + "/" + manifest.trees_dir + "/" +
                             task.tree + ".xml";
    std::shared_ptr<BT::Tree> tree;
    try {
      std::lock_guard<std::mutex> lk(core_->create_mu);  // BT.CPP 共享解析器
      tree = std::make_shared<BT::Tree>(core_->factory.createTreeFromFile(path));
    } catch (const std::exception& e) {
      throw std::runtime_error("loadPack[" + manifest.name + " v" +
                               manifest.version + "] task '" + taskType +
                               "': " + e.what());
    }
    std::vector<std::string> errors;
    const auto defs = parseSlotSchema(task.schema, errors);
    detail::validateTreeKeys(*tree, defs, errors);
    if (!errors.empty()) {
      std::string joined;
      for (const auto& e : errors) joined += "\n  - " + e;
      throw std::runtime_error("loadPack[" + manifest.name + " v" +
                               manifest.version + "] task '" + taskType +
                               "':" + joined);
    }
  }

  {
    std::lock_guard<std::mutex> lk(core_->mu);
    core_->packs.push_back(detail::EngineCore::PackEntry{
        std::move(manifest), manifestDir});
  }
  // M8:pack 声明的失败码登记进引擎(先声明后使用)
  for (const auto& [taskType, task] : manifest.tasks) {
    for (const auto& [code, desc] : task.failure_codes) {
      core_->failure_codes->declare(code, desc);
    }
  }
  core_->logf(LogLevel::Info, "pack loaded (see journal for details)");
}

TaskHandle Engine::submitTask(TaskSpec spec) {
  if (spec.task_id.empty()) {
    spec.task_id = detail::generateTaskId();
  }
  // 恢复重跑自动钉住原 pack 版本:灰度翻转时,恢复必须与原执行版本一致,
  // 否则检查点 id 对不上会导致已完成段被静默重执行
  if (!spec.resume_journal.is_array() || spec.resume_journal.empty()) {
    // 非恢复提交:不处理
  } else if (spec.pack_version.empty()) {
    for (const auto& e : spec.resume_journal) {
      if (e.is_object() && e.value("event", "") == "submitted" &&
          e.contains("data") && e["data"].value("pack_version", "").size() > 0) {
        spec.pack_version = e["data"]["pack_version"].get<std::string>();
        break;
      }
    }
  }
  const detail::EngineCore::PackEntry* entry =
      core_->resolveTask(spec.task_type, spec.pack_version, spec.task_id);
  if (!entry) {
    core_->logf(LogLevel::Error,
                "submitTask: no loaded pack provides task_type '" +
                    spec.task_type + "'");
    return TaskHandle();
  }
  const auto& task = entry->manifest.tasks.at(spec.task_type);
  if (spec.schema.empty()) {
    spec.schema = task.schema;
  }
  if (spec.timeout_ms == 0) {
    spec.timeout_ms = task.timeout_ms;
  }
  // slot_defaults:提交未给的键用 pack 声明的默认值(变体不 fork 的机制)
  for (auto it = task.slot_defaults.begin(); it != task.slot_defaults.end();
       ++it) {
    if (!spec.slots.contains(it.key())) {
      spec.slots[it.key()] = it.value();
    }
  }
  const std::string path = entry->dir + "/" + entry->manifest.trees_dir +
                           "/" + task.tree + ".xml";
  return submitFromFile(path, std::move(spec), entry->manifest.name,
                        entry->manifest.version);
}

void Engine::setCanary(const std::string& packName, const std::string& version,
                       int percent) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->canary = {packName, version, percent};
}

bool Engine::validateTask(BT::Tree& tree, const TaskSpec& spec) const {
  std::vector<std::string> errors;
  const auto defs = parseSlotSchema(spec.schema, errors);
  auto slotErrors = validateSlotsAgainstSchema(defs, spec.slots);
  errors.insert(errors.end(), slotErrors.begin(), slotErrors.end());
  if (!defs.empty()) {
    // schema 未启用时跳过 L2 数据流检查(向后兼容既有任务)
    detail::validateTreeKeys(tree, defs, errors);
  }
  if (errors.empty()) {
    return true;
  }
  for (const auto& e : errors) {
    core_->logf(LogLevel::Error, "validation: " + e);
  }
  return false;
}

std::vector<std::string> Engine::validate(const std::string& xmlText,
                                          const nlohmann::json& schema) {
  std::vector<std::string> errors;
  std::shared_ptr<BT::Tree> tree;
  try {
    std::lock_guard<std::mutex> lk(core_->create_mu);  // BT.CPP 共享解析器
    tree = std::make_shared<BT::Tree>(
        core_->factory.createTreeFromText(xmlText));
  } catch (const std::exception& e) {
    errors.push_back(std::string("parse error: ") + e.what());
    return errors;
  }
  const auto defs = parseSlotSchema(schema, errors);
  detail::validateTreeKeys(*tree, defs, errors);
  return errors;
}

void Engine::setLimits(const EngineLimits& limits) {
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    core_->limits = limits;
  }
  core_->scheduler.setLimits(limits.max_pending_jobs,
                             limits.max_pending_timers);
}

void Engine::advanceTime(uint64_t ms) {
  if (!core_->scheduler.advanceVirtualTime(ms)) {
    core_->logf(LogLevel::Warn,
                "advanceTime ignored: engine is not in virtual-clock mode");
  }
}

void Engine::declareFailureCodes(
    const std::map<std::string, std::string>& codes) {
  for (const auto& [code, desc] : codes) {
    core_->failure_codes->declare(code, desc);
  }
}

void Engine::setWatchdog(std::chrono::milliseconds interval,
                         bool halt_on_trigger, WatchdogCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->watchdog_ms = static_cast<uint64_t>(interval.count());
  core_->watchdog_halt = halt_on_trigger;
  core_->watchdog_cb = std::move(cb);
}

TaskHandle Engine::submitTree(std::shared_ptr<BT::Tree> tree, TaskSpec spec,
                              const std::string& packName,
                              const std::string& packVersion) {
  std::vector<std::string> parseErrors;  // schema 格式问题已在 validateTask 上报
  const auto defs = parseSlotSchema(spec.schema, parseErrors);
  detail::injectSlots(*tree, spec, defs);
  auto ctx = std::make_shared<detail::TaskContext>();
  ctx->task_id = spec.task_id;
  ctx->task_type = spec.task_type;
  ctx->pack_name = packName;
  ctx->pack_version = packVersion;
  ctx->tree = tree;
  // 节点计时:订阅全树状态迁移(相对任务提交时刻);迁移同时刷新活动戳
  ctx->activity->store(
      static_cast<int64_t>(core_->clock->nowMs()));
  ctx->trace = std::make_shared<detail::TaskTraceLogger>(
      tree->rootNode(), ctx->node_stats, ctx->activity, core_->clock);
  // Groot2 实时可视化(M7):端口被占用等异常只告警,不影响任务执行
  if (spec.groot2_port != 0) {
    try {
      ctx->groot2 = std::make_shared<BT::Groot2Publisher>(*tree,
                                                          spec.groot2_port);
    } catch (const std::exception& e) {
      core_->logf(LogLevel::Warn, "groot2 publisher on port " +
                     std::to_string(spec.groot2_port) + " failed: " + e.what());
    }
  }
  {
    // 断点恢复(M2):从上一轮 journal 提取已完成的检查点快照
    std::lock_guard<std::mutex> lk(ctx->journal_mu);
    if (spec.resume_journal.is_array()) {
      for (const auto& e : spec.resume_journal) {
        JournalEvent ev;
        try {
          ev = e.get<JournalEvent>();
        } catch (const std::exception&) {
          continue;
        }
        if (ev.event == "checkpoint" && ev.data.contains("id") &&
            ev.data.contains("blackboard")) {
          ctx->resume_checkpoints[ev.data["id"].get<std::string>()] =
              ev.data["blackboard"];
        }
      }
    }
  }
  // 资源上限(M4)+ 资源仲裁(M5):超限/资源冲突 → 拒绝提交
  std::string rejectReason;
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    if (core_->limits.max_active_tasks > 0 &&
        core_->active.size() >= core_->limits.max_active_tasks) {
      rejectReason = "max_active_tasks=" +
                     std::to_string(core_->limits.max_active_tasks) +
                     " reached";
    } else if (!spec.resources.empty()) {
      for (const auto& [tid, held] : core_->task_resources) {
        for (const auto& r : spec.resources) {
          if (std::find(held.begin(), held.end(), r) != held.end()) {
            rejectReason =
                "resource '" + r + "' held by task '" + tid + "'";
            break;
          }
        }
        if (!rejectReason.empty()) break;
      }
    }
    if (rejectReason.empty()) {
      core_->active[spec.task_id] = ctx;
      if (!spec.resources.empty()) {
        core_->task_resources[spec.task_id] = spec.resources;
      }
    }
  }
  if (!rejectReason.empty()) {
    core_->logf(LogLevel::Error,
                "submit rejected: " + rejectReason + " (task '" +
                    spec.task_id + "')");
    return TaskHandle();
  }
  TaskHandle handle(ctx, core_);
  core_->appendJournal(
      ctx, "submitted",
      nlohmann::json{{"task_type", spec.task_type},
                     {"pack", packName},
                     {"pack_version", packVersion},
                     {"resumed",
                      spec.resume_journal.is_array() &&
                          !spec.resume_journal.empty()}});
  auto core = core_;
  auto id = spec.task_id;
  if (!core_->scheduler.post([core, id] { core->tickTask(id); })) {
    // 首次 tick 投递被拒(队列满/已停止):撤销注册,不留僵尸 Queued
    // 任务占着 active 槽位和资源名额
    {
      std::lock_guard<std::mutex> lk(core_->mu);
      core_->active.erase(spec.task_id);
      core_->task_resources.erase(spec.task_id);
    }
    core_->logf(LogLevel::Error,
                "submit rejected: scheduler queue full or stopped (task '" +
                    spec.task_id + "')");
    return TaskHandle();
  }

  // 策略:任务级超时(调度线程回调;到点未终态则 halt + TASK_TIMEOUT)
  if (spec.timeout_ms > 0) {
    const uint64_t timeoutMs = spec.timeout_ms;
    core_->scheduler.callLater(std::chrono::milliseconds(timeoutMs),
                               [core, ctx, timeoutMs] {
      if (ctx->finished.load()) {
        return;
      }
      FailureRecord fr;
      fr.code = "TASK_TIMEOUT";
      fr.node = "engine";
      fr.node_type = "Engine";
      fr.message = "task exceeded " + std::to_string(timeoutMs) + " ms";
      fr.timestamp = nowTimestamp();
      ctx->failures->add(std::move(fr));
      ctx->tree->haltTree();
      core->finishTask(ctx, TaskState::Failed, "task timeout");
    });
  }

  // 策略:任务 watchdog(M4)。spec.watchdog_ms > 0 覆盖引擎默认
  uint64_t watchdogMs = 0;
  bool haltOnWatchdog = false;
  {
    std::lock_guard<std::mutex> lk(core_->mu);
    watchdogMs = spec.watchdog_ms > 0 ? spec.watchdog_ms : core_->watchdog_ms;
    haltOnWatchdog = core_->watchdog_halt;
  }
  if (watchdogMs > 0) {
    core_->armWatchdog(ctx, watchdogMs);
    (void)haltOnWatchdog;  // halt 行为在触发时读取 core_->watchdog_halt
  }
  return handle;
}

void Engine::onFinished(FinishedCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->on_finished = std::move(cb);
}

void Engine::setJournalSink(JournalSink cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->journal_sink = std::move(cb);
}

void Engine::setLogger(LogCallback cb) {
  std::lock_guard<std::mutex> lk(core_->mu);
  core_->log = std::move(cb);
}

} // namespace nebulabt
