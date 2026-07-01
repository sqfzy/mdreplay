#pragma once
// clock.hpp — 回放节奏:绝对虚拟时钟,实际延迟 = (ts - ts0) × realtime,不累积漂移。
//
// realtime ∈ [0,1]:值=有多实时。0 = 尽快(no-op,纯逻辑序、完全可复现);1 = 原速;0.5 = 2× 实时。
// 关键:target 从绝对量 (ts - ts0) 算,不逐步累加 delta —— 某拍被调度晚了,下一拍自动追回。
// 墙钟用 steady_clock(单调、免疫 NTP 跳变);墙钟原点 wall0 = 首拍时刻(now)。
//
// 数据原点 ts0(= 对齐到首拍墙钟 now 的那个数据时刻):
//   · 默认(无 init_ts):锚定**首个被回放的事件**(= 窗口内最早,因窗口过滤在归并层、早于时钟)。
//   · init_ts 指定:手动把该 data-ts 当 t0(构造即定);早于 t0 的事件偏移钳到 0(立即播,不回到过去)。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

namespace mdreplay {

class Clock {
public:
  // init_ts:手动数据原点(epoch ns)。缺省 → 自动锚首个被回放的事件。
  explicit Clock(double realtime, std::optional<std::int64_t> init_ts = std::nullopt) noexcept
      : realtime_(realtime) {
    if (init_ts) { ts0_ = *init_ts; have_ts0_ = true; }  // 手动数据原点:构造即定 ts0
  }

  // 纯函数(可单测):相对 ts0 应有的延迟(ns)。ts0 未定(自动模式首事件前)→ 0;早于 ts0 → 0(立即播)。
  [[nodiscard]] std::int64_t offset_ns(std::int64_t ts_ns) const noexcept {
    if (!have_ts0_) return 0;
    const auto off = static_cast<std::int64_t>(static_cast<double>(ts_ns - ts0_) * realtime_);
    return off > 0 ? off : 0;
  }

  // 阻塞到该事件应发的墙钟时刻。realtime<=0 即不限速直接返回。分块睡 + 查 stop:绝对 target 不变
  // (自校正照旧),但收到停止信号能在 ≤kTick 内打断长 pacing 间隔。
  void pace_to(std::int64_t ts_ns, const std::atomic<bool>& stop) noexcept {
    if (realtime_ <= 0.0) return;
    if (!started_) {  // 首拍:墙钟原点=now;自动模式此刻把 ts0 定为首事件
      wall0_ = std::chrono::steady_clock::now();
      if (!have_ts0_) { ts0_ = ts_ns; have_ts0_ = true; }
      started_ = true;
    }
    sleep_until_or_stop(wall0_ + std::chrono::nanoseconds(offset_ns(ts_ns)), stop);
  }

  [[nodiscard]] bool started() const noexcept { return started_; }

private:
  // 睡到 target + 每 ≤kTick 查一次 stop → 长间隔也能被信号即时打断。
  template <class TimePoint>
  static void sleep_until_or_stop(TimePoint target, const std::atomic<bool>& stop) noexcept {
    using ClockT = typename TimePoint::clock;
    constexpr auto kTick = std::chrono::milliseconds(100);
    while (!stop.load(std::memory_order_relaxed)) {
      const auto now = ClockT::now();
      if (now >= target) return;
      const auto remain = target - now;
      std::this_thread::sleep_for(remain < kTick ? remain : kTick);
    }
  }

  double                                realtime_;
  bool                                  have_ts0_{false};
  bool                                  started_{false};
  std::int64_t                          ts0_{0};
  std::chrono::steady_clock::time_point wall0_{};
};

}  // namespace mdreplay
