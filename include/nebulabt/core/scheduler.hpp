#pragma once
/// 事件驱动调度器:框架中唯一的 tick 驱动源。
///
/// - post(job):将 job 投递到调度线程执行(任意线程可调,线程安全)
/// - callLater(delay, job):定时事件
/// 没有事件时线程休眠——框架不轮询,业务不轮询。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace nebulabt {

class Scheduler {
public:
  using Clock = std::chrono::steady_clock;
  using Job = std::function<void()>;

  ~Scheduler();

  void start();
  void stop();
  bool running() const { return running_.load(); }

  /// 线程安全;job 在调度线程串行执行
  void post(Job job);

  /// 线程安全;delay 毫秒后在调度线程执行 job
  void callLater(std::chrono::milliseconds delay, Job job);

private:
  struct Timer {
    Clock::time_point when;
    uint64_t seq;
    Job job;
  };
  struct TimerCmp {
    bool operator()(const Timer& a, const Timer& b) const { return a.when > b.when; }
  };

  void loop();
  void runGuarded(const Job& job);

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::priority_queue<Timer, std::vector<Timer>, TimerCmp> timers_;
  uint64_t timer_seq_ = 0;
  bool stopped_ = true;              // mu_ 保护
  std::atomic<bool> running_{false};
};

} // namespace nebulabt
