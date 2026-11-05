#pragma once
/// 时钟抽象(M6):确定性内核的地基。
///
/// 引擎内所有时间敏感策略——任务超时、RetryWithDelay 延时、watchdog
/// 静默计算、活动戳——统一走 NebulaClock:
/// - SteadyClock(默认):真实单调时钟,生产行为不变
/// - VirtualClock:测试用。手动 advance(ms) 推进,到期定时器随即触发
///   ——重试/超时/看门狗场景零真实等待,且同一 journal 重放可复现
///
/// 虚拟模式语义:advance 把"虚拟时间已到期"的定时器**物化为调度作业**
/// (仍由调度线程串行执行),单线程 tick 模型不变;journal 时间戳仍用
/// 真实墙钟(落盘记录保持真实)。

#include <atomic>
#include <chrono>
#include <memory>

namespace nebulabt {

class NebulaClock {
public:
  virtual ~NebulaClock() = default;
  /// 单调毫秒(与 steady_clock 同源语义)
  virtual uint64_t nowMs() = 0;
};

/// 默认实现:真实单调时钟
class SteadyClock : public NebulaClock {
public:
  uint64_t nowMs() override {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
};

/// 测试实现:虚拟时钟,手动推进
class VirtualClock : public NebulaClock {
public:
  uint64_t nowMs() override { return now_.load(std::memory_order_relaxed); }

  /// 推进虚拟时间(到期定时器由 Scheduler::advanceVirtualTime 物化)
  void advance(uint64_t ms) { now_.fetch_add(ms, std::memory_order_relaxed); }

private:
  std::atomic<uint64_t> now_{0};
};

} // namespace nebulabt
