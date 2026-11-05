#include "nebulabt/core/scheduler.hpp"

#include <algorithm>
#include <cstdio>

namespace nebulabt {

namespace {
uint64_t steadyNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
} // namespace

Scheduler::~Scheduler() {
  stop();
}

void Scheduler::setVirtualClock(std::shared_ptr<VirtualClock> clock) {
  std::lock_guard<std::mutex> lk(mu_);
  vclock_ = std::move(clock);
}

void Scheduler::start() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!stopped_) {
      return; // already running
    }
    stopped_ = false;
  }
  running_.store(true);
  thread_ = std::thread(&Scheduler::loop, this);
}

void Scheduler::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
}

void Scheduler::setLimits(size_t max_jobs, size_t max_timers) {
  std::lock_guard<std::mutex> lk(mu_);
  max_jobs_ = max_jobs;
  max_timers_ = max_timers;
}

bool Scheduler::post(Job job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_ || draining_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::post after stop/during drain, job dropped\n");
      return false;
    }
    if (max_jobs_ > 0 && jobs_.size() >= max_jobs_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::post rejected: queue full (%zu >= %zu)\n",
                   jobs_.size(), max_jobs_);
      return false;
    }
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
  return true;
}

bool Scheduler::callLater(std::chrono::milliseconds delay, Job job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_ || draining_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::callLater after stop/during drain, job dropped\n");
      return false;
    }
    if (max_timers_ > 0 && timers_.size() >= max_timers_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::callLater rejected: timers full (%zu >= %zu)\n",
                   timers_.size(), max_timers_);
      return false;
    }
    const uint64_t now =
        vclock_ ? vclock_->nowMs()
                : steadyNowMs();
    timers_.push(Timer{now + static_cast<uint64_t>(delay.count()),
                       ++timer_seq_, std::move(job)});
  }
  cv_.notify_one();
  return true;
}

bool Scheduler::advanceVirtualTime(uint64_t ms) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!vclock_ || stopped_ || draining_) {
      return false;
    }
  }
  // 相位 1:在"当前"虚拟时刻排空已排队作业(如 submit 的首次 tick)。
  // 否则它们会在时钟推进后才执行,活动戳等时间观测被记到未来
  {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return jobs_.empty() && in_flight_.load() == 0; });
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    vclock_->advance(ms);
    const uint64_t now = vclock_->nowMs();
    std::vector<Job> due;
    while (!timers_.empty() && timers_.top().deadline_ms <= now) {
      due.push_back(timers_.top().job);
      timers_.pop();
    }
    // 到期定时器按(到期时间,注册序)插队:它们在虚拟时间上更早
    jobs_.insert(jobs_.begin(), std::make_move_iterator(due.rbegin()),
                 std::make_move_iterator(due.rend()));
  }
  cv_.notify_one();
  {
    // 相位 2:等待到期作业及其级联(它们可能 post 后续作业/注册新定时器)
    // 全部执行完:advance 返回后确定性成立,测试无需真实 sleep。
    // 虚拟模式下定时器不经真实时间触发,级联必然有穷。
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return jobs_.empty() && in_flight_.load() == 0; });
  }
  return true;
}

void Scheduler::drainAndStop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_) {
      return;
    }
    draining_ = true;  // 拒绝新投递;调度线程继续排空队列
    // 未触发定时器直接丢弃:销毁路径上的策略定时器已无意义
    timers_ = {};
  }
  cv_.notify_all();
  {
    // 等队列排空且在飞 job 完成(它们可能投递收尾作业,已拒绝但无碍)
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return jobs_.empty() && in_flight_.load() == 0; });
    stopped_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
}

void Scheduler::loop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (true) {
    if (stopped_) {
      return;
    }
    if (!jobs_.empty()) {
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      lk.unlock();
      runGuarded(job);
      lk.lock();
      continue;
    }
    // 虚拟时钟模式下定时器永远不经真实时间触发(只由 advanceVirtualTime
    // 物化为作业);真实模式由调度线程等待最近到期定时器
    if (!vclock_ && !timers_.empty()) {
      const uint64_t now = steadyNowMs();
      const uint64_t next = timers_.top().deadline_ms;
      if (now >= next) {
        auto timer = timers_.top();
        timers_.pop();
        lk.unlock();
        runGuarded(timer.job);
        lk.lock();
        continue;
      }
      const auto nextTp =
          std::chrono::steady_clock::time_point(
              std::chrono::milliseconds(next));
      cv_.wait_until(lk, nextTp);
      continue;
    }
    cv_.wait(lk);
  }
}

void Scheduler::runGuarded(const Job& job) {
  in_flight_.fetch_add(1);
  try {
    if (job) {
      job();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[nebulabt] Scheduler job exception: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[nebulabt] Scheduler job unknown exception\n");
  }
  in_flight_.fetch_sub(1);
  cv_.notify_all();  // drainAndStop 的等待方
}

} // namespace nebulabt
