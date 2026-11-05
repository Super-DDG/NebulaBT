#include "nebulabt/core/scheduler.hpp"

#include <cstdio>

namespace nebulabt {

Scheduler::~Scheduler() {
  stop();
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

void Scheduler::post(Job job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_ || draining_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::post after stop/during drain, job dropped\n");
      return;
    }
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void Scheduler::callLater(std::chrono::milliseconds delay, Job job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_ || draining_) {
      std::fprintf(stderr,
                   "[nebulabt] Scheduler::callLater after stop/during drain, job dropped\n");
      return;
    }
    timers_.push(Timer{Clock::now() + delay, ++timer_seq_, std::move(job)});
  }
  cv_.notify_one();
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
    if (!timers_.empty()) {
      const auto now = Clock::now();
      const auto next = timers_.top().when;
      if (now >= next) {
        auto timer = timers_.top();
        timers_.pop();
        lk.unlock();
        runGuarded(timer.job);
        lk.lock();
        continue;
      }
      cv_.wait_until(lk, next);
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
