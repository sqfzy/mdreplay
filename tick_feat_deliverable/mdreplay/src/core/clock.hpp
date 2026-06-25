#pragma once
// clock.hpp — 回放节奏:绝对虚拟时钟,实际延迟 = (ts - ts0) × realtime,锚定首事件不累积漂移。
//
// realtime ∈ [0,1]:值=有多实时。0 = 尽快(no-op,纯逻辑序、完全可复现);1 = 原速;0.5 = 2× 实时。
// 关键:target 从绝对量 (ts - ts0) 算,不逐步累加 delta —— 某拍被调度晚了,下一拍自动追回。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace mdreplay {

class Clock {
public:
  explicit Clock(double realtime) noexcept : realtime_(realtime) {}

  // 纯函数核心(可单测,无副作用):相对锚点应有的延迟(ns)。未锚定返回 0。
  [[nodiscard]] std::int64_t offset_ns(std::int64_t ts_ns) const noexcept {
    if (!anchored_) return 0;
    return static_cast<std::int64_t>(static_cast<double>(ts_ns - ts0_) * realtime_);
  }

  // 阻塞到该事件应发的墙钟时刻。realtime<=0 即不限速直接返回。
  // 分块睡(每块 ≤kTick)并查 stop:绝对目标 target 不变(自校正语义照旧),但收到停止信号能在
  // ≤kTick 内打断长 pacing 间隔(否则 realtime=1 下两事件相隔数秒时 Ctrl-C 要等满那几秒)。
  void pace_to(std::int64_t ts_ns, const std::atomic<bool>& stop) noexcept {
    if (realtime_ <= 0.0) return;
    if (!anchored_) { ts0_ = ts_ns; wall0_ = std::chrono::steady_clock::now(); anchored_ = true; return; }
    const auto target = wall0_ + std::chrono::nanoseconds(offset_ns(ts_ns));
    constexpr auto kTick = std::chrono::milliseconds(100);
    while (!stop.load(std::memory_order_relaxed)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= target) return;
      const auto remain = target - now;
      std::this_thread::sleep_for(remain < kTick ? remain : kTick);
    }
  }

  [[nodiscard]] bool anchored() const noexcept { return anchored_; }

private:
  double                                 realtime_;
  bool                                   anchored_{false};
  std::int64_t                           ts0_{0};
  std::chrono::steady_clock::time_point  wall0_{};
};

}  // namespace mdreplay
