#include "nebulabt/core/task.hpp"

#include "engine_core.hpp"
#include "node_stats.hpp"
#include <behaviortree_cpp/basic_types.h>
#include <condition_variable>
#include <mutex>

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
  std::unique_lock<std::mutex> lk(ctx_->wait_mu);
  ctx_->wait_cv.wait_for(lk, timeout,
                         [this] { return isTerminal(ctx_->state.load()); });
  return ctx_->state.load();
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
      auto ctx = ctx_;
      std::mutex m;
      std::condition_variable cv;
      bool done = false;
      nlohmann::json snap;
      core_->scheduler.post([&ctx, &snap, &done, &cv, &m] {
        if (ctx->tree && ctx->tree->rootNode()) {
          snap = detail::explainTreeNode(ctx->tree->rootNode(), "",
                                         ctx->node_stats.get());
        }
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_all();
      });
      std::unique_lock<std::mutex> lk(m);
      // 2s 上限:从调度线程自身调用会死锁在此超时上(返回无树快照)
      cv.wait_for(lk, std::chrono::seconds(2), [&] { return done; });
      if (done) {
        j["tree"] = std::move(snap);
      } else {
        j["tree_snapshot"] = "unavailable (called from scheduler thread?)";
      }
    }
  }

  j["failures"] = failures();
  return j;
}

} // namespace nebulabt
