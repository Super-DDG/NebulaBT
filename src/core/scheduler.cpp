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
    if (stopped_) {
      std::fprintf(stderr, "[nebulabt] Scheduler::post after stop, job dropped\n");
      return;
    }
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void Scheduler::callLater(std::chrono::milliseconds delay, Job job) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_) {
      std::fprintf(stderr, "[nebulabt] Scheduler::callLater after stop, job dropped\n");
      return;
    }
    timers_.push(Timer{Clock::now() + delay, ++timer_seq_, std::move(job)});
  }
  cv_.notify_one();
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
  try {
    if (job) {
      job();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[nebulabt] Scheduler job exception: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[nebulabt] Scheduler job unknown exception\n");
  }
}

} // namespace nebulabt
