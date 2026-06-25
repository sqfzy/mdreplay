#pragma once
// signal.hpp — 优雅退出:SIGINT/SIGTERM 只置一个原子标志(async-signal-safe,处理器里不碰锁/IO),
// 回放循环每拍查它、Clock 的 pacing sleep 分块查它 → 收到信号即在干净边界停,跑完收尾
// (reporter.finish() + skips.log_summary())、可控退出(0),而非被硬杀丢失收尾与退出码。
//
// 为何 atomic<bool> 而非 volatile sig_atomic_t:前者在所有真实平台 lock-free,信号处理器里
// 只做一次 relaxed store 同样 async-signal-safe,读侧语义更清晰。

#include <atomic>
#include <csignal>

namespace mdreplay {

inline std::atomic<bool> g_stop_requested{false};

// 信号处理器:唯一允许的动作是置标志(不可 log/不可分配/不可加锁)。
inline void request_stop(int /*signum*/) noexcept {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

inline void install_signal_handlers() {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
}

[[nodiscard]] inline bool stop_requested() noexcept {
  return g_stop_requested.load(std::memory_order_relaxed);
}

}  // namespace mdreplay
