#pragma once
/// 事件驱动调度器:框架中唯一的 tick 驱动源。
///
/// - post(job):将 job 投递到调度线程执行(任意线程可调,线程安全)
/// - callLater(delay, job):定时事件
/// 没有事件时线程休眠——框架不轮询,业务不轮询。
///
/// 虚拟时钟模式(M6):setVirtualClock 后,定时器不再由真实时间触发,
/// 而是随 advanceVirtualTime(ms) 推进虚拟时间、把到期定时器物化为调度
/// 作业(仍由调度线程串行执行)。测试中重试/超时/看门狗零真实等待。

#include "nebulabt/core/clock.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
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

  /// 启用虚拟时钟模式(须在 start() 之前调用;clock 不可为空)。
  /// 启用后 callLater 的定时器只在 advanceVirtualTime 推进时触发。
  void setVirtualClock(std::shared_ptr<VirtualClock> clock);

  /// 资源上限(M4):0 = 不限。超限时投递被拒绝并告警,防止失控动作
  /// 回调风暴无界增长吃掉嵌入式内存
  void setLimits(size_t max_jobs, size_t max_timers);

  /// 线程安全;job 在调度线程串行执行。返回 false = 被拒绝(超限/已停止)
  bool post(Job job);

  /// 线程安全;delay 毫秒后在调度线程执行 job。返回 false = 被拒绝
  bool callLater(std::chrono::milliseconds delay, Job job);

  /// 排空后停止:不再接受新任务,等队列清空且在飞 job 完成后 join。
  /// 未触发的定时器被丢弃(销毁路径上的策略定时器已无意义)。
  /// 用于 Engine 析构:保证已投递的收尾作业(如取消任务)真正执行。
  void drainAndStop();

  /// 仅虚拟时钟模式:推进虚拟时间并把到期定时器按(到期时间,注册序)
  /// 物化为调度作业。真实时钟模式返回 false。
  bool advanceVirtualTime(uint64_t ms);

private:
  struct Timer {
    uint64_t deadline_ms;  // 真实模式:steady 毫秒;虚拟模式:虚拟毫秒
    uint64_t seq;
    Job job;
  };
  struct TimerCmp {
    bool operator()(const Timer& a, const Timer& b) const {
      if (a.deadline_ms != b.deadline_ms) {
        return a.deadline_ms > b.deadline_ms;
      }
      return a.seq > b.seq;  // 同刻到期按注册序触发
    }
  };

  void loop();
  void runGuarded(const Job& job);

  std::shared_ptr<VirtualClock> vclock_;  // 非空 = 虚拟时钟模式
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::priority_queue<Timer, std::vector<Timer>, TimerCmp> timers_;
  uint64_t timer_seq_ = 0;
  size_t max_jobs_ = 0;              // mu_ 保护;0 = 不限
  size_t max_timers_ = 0;            // mu_ 保护;0 = 不限
  bool stopped_ = true;              // mu_ 保护
  bool draining_ = false;            // mu_ 保护:拒绝新投递,等队列排空
  std::atomic<int> in_flight_{0};    // 正在执行的 job 数
  std::atomic<bool> running_{false};
};

} // namespace nebulabt
