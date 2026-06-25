#pragma once

#include <atomic>
#include <cstdint>

// 单写者高频覆盖、多读者并发读。读者要拿到一份"完整一致"的数据,但又不能加锁
// 机制靠一个序号 seq:
// - 偶数 = 数据稳定可读
// - 奇数 = 正在写
// - 每写一次 +2(奇→偶 各一步)
// 读者读前读后各看一次 seq:两次相同且为偶 ⇒ 这段时间没人写,数据一致;否则撕裂,重来

namespace gconf::shm::v2::seqlock {

// ── writer ───────────────────────────────────────────────────────────────────
// 写入 payload 前调用,`writing` 为奇(写入中)标记。前沿 release fence 确保后续 payload
// 写【不会】重排到本标记之前(修 #10 的关键)。
inline void begin(std::atomic<std::uint64_t> &seq, std::uint64_t writing) noexcept {
  seq.store(writing, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}
// 写完 payload 后调用,`done` 为偶(可读)标记。release store 确保 payload 先于本标记可见(后沿)。
inline void end(std::atomic<std::uint64_t> &seq, std::uint64_t done) noexcept { seq.store(done, std::memory_order_release); }

// ── reader ───────────────────────────────────────────────────────────────────
// 读 payload 前:取一次 seq(acquire)。返回值传给 verify()。
[[nodiscard]] inline std::uint64_t snapshot(const std::atomic<std::uint64_t> &seq) noexcept { return seq.load(std::memory_order_acquire); }
// 读完 payload 后:acquire fence(确保 payload 读不越过第二次 seq load)+ 重取比对。
// 返回 true ⇔ 期间没有写者介入(payload 一致)。调用方还需自查 s1 为偶(非写入中)。
[[nodiscard]] inline bool verify(const std::atomic<std::uint64_t> &seq, std::uint64_t s1) noexcept {
  std::atomic_thread_fence(std::memory_order_acquire);
  return seq.load(std::memory_order_acquire) == s1;
}

} // namespace gconf::shm::v2::seqlock
