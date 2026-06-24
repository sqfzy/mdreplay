#pragma once
// gconf/shm/v2/seqlock.h — v2 唯一正确的 seqlock 内存序原语(board 与 bcast ring 共用)。
//
// 修 v1 #10:v1 writer 在奇-seq store(release)与 payload 写之间【缺前沿 StoreStore 屏障】——
// release 只挡"之前的操作后移",不挡"之后的 payload 前移"。x86(TSO)良性,aarch64 弱序上
// payload 可重排到奇-seq 之前 → reader 读到偶-seq(旧)却已变的 payload = 未检测撕裂读。
//
// 正确序(参 Linux memory-barriers.txt / seqlock.h、Vyukov):
//   writer: seq=奇(relaxed) → fence(release) → payload → seq=偶(release)
//   reader: s1=seq(acquire); 若奇 retry → payload → fence(acquire) → s2=seq; 一致 ⇔ s1==s2 且偶
//
// seq 取值方案由调用方定(board:偶/奇计数 +1/+2;bcast:槽编码 (slot<<1)|writing)——本原语只负责
// 把 fence 放对,不关心具体数值。

#include <atomic>
#include <cstdint>

namespace gconf::shm::v2::seqlock {

// ── writer ───────────────────────────────────────────────────────────────────
// 写入 payload 前调用,`writing` 为奇(写入中)标记。前沿 release fence 确保后续 payload
// 写【不会】重排到本标记之前(修 #10 的关键)。
inline void begin(std::atomic<std::uint64_t>& seq, std::uint64_t writing) noexcept {
  seq.store(writing, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
}
// 写完 payload 后调用,`done` 为偶(可读)标记。release store 确保 payload 先于本标记可见(后沿)。
inline void end(std::atomic<std::uint64_t>& seq, std::uint64_t done) noexcept {
  seq.store(done, std::memory_order_release);
}

// ── reader ───────────────────────────────────────────────────────────────────
// 读 payload 前:取一次 seq(acquire)。返回值传给 verify()。
[[nodiscard]] inline std::uint64_t snapshot(const std::atomic<std::uint64_t>& seq) noexcept {
  return seq.load(std::memory_order_acquire);
}
// 读完 payload 后:acquire fence(确保 payload 读不越过第二次 seq load)+ 重取比对。
// 返回 true ⇔ 期间没有写者介入(payload 一致)。调用方还需自查 s1 为偶(非写入中)。
[[nodiscard]] inline bool verify(const std::atomic<std::uint64_t>& seq, std::uint64_t s1) noexcept {
  std::atomic_thread_fence(std::memory_order_acquire);
  return seq.load(std::memory_order_acquire) == s1;
}

}  // namespace gconf::shm::v2::seqlock
