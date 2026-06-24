#pragma once
// gconf/shm/v2/req.h — v2 策略→order 请求队列(per-venue 可靠 SPSC + 背压)。
//
// 范式:io_uring SQ/CQ(mmap 共享 SPSC + head/tail + release/acquire)。相对 v1:
//   v1 PrivateReqRing 是 SPMC 广播环(绕圈覆盖)→ 策略写快于 order 消费时【静默丢单】,且物理
//   不可恢复(被覆盖的 client_order_id 没了,无从回 REJECTED)。
//   v2 改 per-venue SPSC(策略按 venue 路由到各自队列)+ 背压:消费者 tail 回写 SHM,生产者写前
//   查满 → 满则 try_push 返回 false(【不覆盖】)→ 生产者本地 REJECTED → 根除静默丢单。
//
// 帧格式复用 v1 gconf::priv::PrivateReqFrame(64B POD,已够好);v2 只改队列机制。
// SPSC 无 per-entry seqlock:payload 由 head/tail 的 release/acquire 定序。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/req_frame.h>   // gconf::priv::PrivateReqFrame
#include <gconf/shm/v2/seg_header.h>

namespace gconf::shm::v2 {

inline constexpr std::size_t REQ_QUEUE_CAP = 1024;  // 2 的幂
inline constexpr std::uint64_t kReqSchemaHash = schema_fnv(
    "ReqQueue:PrivateReqFrame64:type,account,ex,ac,target_path,submit,union{place,modify,query}");

// 可靠 SPSC 队列:段头 + producer head + consumer tail(各独占 cache line,防 false sharing)+ entries。
template <class Entry, std::size_t Cap>
struct SpscQueue {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");
  static constexpr std::size_t MASK = Cap - 1;

  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> head{0};   // 生产者推进
  alignas(64) std::atomic<std::uint64_t> tail{0};   // 消费者回写(背压用)
  alignas(64) Entry entries[Cap];

  // 生产者(策略):入队。返回 false = 满 → 调用方本地 REJECTED(【绝不覆盖】)。
  template <class FillFn>
  [[nodiscard]] bool try_push(FillFn&& fn) noexcept {
    const std::uint64_t h = head.load(std::memory_order_relaxed);
    const std::uint64_t t = tail.load(std::memory_order_acquire);
    if (h - t >= Cap) return false;                  // 满 → 不写,不覆盖
    fn(entries[h & MASK]);                            // 写 payload
    head.store(h + 1, std::memory_order_release);     // 发布(payload 先于 head 可见)
    return true;
  }

  // 消费者(order):drain 全部就绪帧,fn(const Entry&);消费后回写 tail(背压可见)。返回消费条数。
  template <class Fn>
  std::size_t drain(Fn&& fn) noexcept {
    const std::uint64_t h = head.load(std::memory_order_acquire);
    std::uint64_t t = tail.load(std::memory_order_relaxed);
    std::size_t n = 0;
    for (; t < h; ++t, ++n) fn(entries[t & MASK]);
    if (n) tail.store(t, std::memory_order_release);  // 发布已消费(生产者据此判满)
    return n;
  }

  // 诊断:当前在飞条数(near-instant snapshot)。
  [[nodiscard]] std::uint64_t size() const noexcept {
    return head.load(std::memory_order_relaxed) - tail.load(std::memory_order_relaxed);
  }
};

using ReqQueue = SpscQueue<gconf::priv::PrivateReqFrame, REQ_QUEUE_CAP>;
static_assert(std::is_standard_layout_v<ReqQueue>, "ReqQueue must be standard-layout (SHM contract)");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SHM head/tail atomics must be lock-free — SPSC 跨进程定序前提(#90)");

}  // namespace gconf::shm::v2
