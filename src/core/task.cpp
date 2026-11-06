#include "nebulabt/core/task.hpp"

#include "engine_core.hpp"
#include "node_stats.hpp"
#include <behaviortree_cpp/basic_types.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace nebulabt {

const char* toString(TaskState s) {
  switch (s) {
    case TaskState::Queued:    return "QUEUED";
    case TaskState::Running:   return "RUNNING";
    case TaskState::Succeeded: return "SUCCEEDED";
    case TaskState::Failed:    return "FAILED";
    case TaskState::Canceled:  return "CANCELED";
  }
  return "UNKNOWN";
}

// ============================================================================
// TaskHandle
// ============================================================================

TaskHandle::TaskHandle(std::shared_ptr<detail::TaskContext> ctx,
                       std::shared_ptr<detail::EngineCore> core)
  : ctx_(std::move(ctx)), core_(std::move(core)) {}

std::string TaskHandle::taskId() const {
  return ctx_ ? ctx_->task_id : std::string();
}

TaskState TaskHandle::state() const {
  return ctx_ ? ctx_->state.load() : TaskState::Failed;
}

TaskState TaskHandle::wait() {
  if (!ctx_) {
    return TaskState::Failed;
  }
  std::unique_lock<std::mutex> lk(ctx_->wait_mu);
  ctx_->wait_cv.wait(lk, [this] { return isTerminal(ctx_->state.load()); });
  return ctx_->state.load();
}

TaskState TaskHandle::waitFor(std::chrono::milliseconds timeout) const {
  if (!ctx_) {
    return TaskState::Failed;
  }
  // 原子轮询而非 cv.wait_for:语义相同(1ms 粒度),但 GCC 11 TSan 对
  // timed cv wait 建模不全,会把跨线程锁序误报成 double-lock/race
  // (20 行标准库最小复现实证);wait() 无超时版保留 cv 阻塞
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const TaskState s = ctx_->state.load(std::memory_order_acquire);
    if (isTerminal(s) || std::chrono::steady_clock::now() >= deadline) {
      return s;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool TaskHandle::cancel() {
  if (!ctx_ || isTerminal(ctx_->state.load())) {
    return false;
  }
  auto core = core_;
  auto ctx = ctx_;
  core->scheduler.post([core, ctx] {
    if (ctx->finished.load()) {
      return;
    }
    ctx->tree->haltTree();
    core->finishTask(ctx, TaskState::Canceled, "canceled by request");
  });
  return true;
}

std::vector<FailureRecord> TaskHandle::failures() const {
  if (ctx_ && ctx_->failures) {
    return ctx_->failures->snapshot();
  }
  return {};
}

std::vector<JournalEvent> TaskHandle::journal() const {
  if (!ctx_) {
    return {};
  }
  std::lock_guard<std::mutex> lk(ctx_->journal_mu);
  return ctx_->journal;
}

nlohmann::json TaskHandle::explain() const {
  if (!ctx_) {
    return nlohmann::json();
  }
  nlohmann::json j;
  j["task_id"] = ctx_->task_id;
  j["task_type"] = ctx_->task_type;
  if (!ctx_->pack_name.empty()) {
    j["pack"] = ctx_->pack_name;
    j["pack_version"] = ctx_->pack_version;
  }
  j["state"] = toString(ctx_->state.load());

  // 树遍历无锁,与 tick 并发读 BT.CPP 内部状态不安全:
  // 终态后直接遍历;RUNNING 中向调度线程投递快照 job,在 tick 线程
  // 内完成遍历(M7:排障恰好在任务卡住时最需要 explain)
  if (ctx_->tree && ctx_->tree->rootNode()) {
    if (ctx_->finished.load()) {
      j["tree"] = detail::explainTreeNode(ctx_->tree->rootNode(), "",
                                          ctx_->node_stats.get());
    } else if (core_) {
      // 快照状态必须共享:explain() 超时返回后 job 仍会(迟到)执行,
      // 栈上引用会悬垂成 UB;job 与等待方经 shared_ptr 安全交接。
      // 等待用原子标志 + 轮询(1ms),不用 condition_variable::wait_for:
      // GCC 11 TSan 对 timed cv 建模不全,会对该模式整链误报
      struct SnapState {
        std::mutex mu;                    // 只保护 snap
        nlohmann::json snap;
        std::atomic<bool> done{false};
      };
      auto ctx = ctx_;
      auto st = std::make_shared<SnapState>();
      if (core_->scheduler.post([ctx, st] {
            nlohmann::json snap;
            if (ctx->tree && ctx->tree->rootNode()) {
              snap = detail::explainTreeNode(ctx->tree->rootNode(), "",
                                             ctx->node_stats.get());
            }
            {
              std::lock_guard<std::mutex> lk(st->mu);
              st->snap = std::move(snap);
            }
            st->done.store(true, std::memory_order_release);
          })) {
        // 2s 上限:从调度线程自身调用时 job 无法执行(返回无树快照)
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!st->done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (st->done.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lk(st->mu);
          j["tree"] = std::move(st->snap);
        } else {
          j["tree_snapshot"] =
              "unavailable (scheduler busy or called from scheduler thread)";
        }
      } else {
        // 投递被拒(队列满/已停止):立即降级,不再白等 2s
        j["tree_snapshot"] = "unavailable (scheduler queue full or stopped)";
      }
    }
  }

  j["failures"] = failures();
  return j;
}

} // namespace nebulabt
