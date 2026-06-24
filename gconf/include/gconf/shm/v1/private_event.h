#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// Private (fill / order-update) event SHM contract — bn-standard.
//
// PrivateEventEntry: 64B = 1 cache line, seqlock via slot-encoded seq.
// PrivateEventRing<Entry,Cap>: MPMC broadcast ring (head fetch_add; each consumer
// holds its OWN tail off-SHM and scans [tail..head]; entries overwrite on wrap).
// The SAME ring template backs order_event.h (OrderEventEntry).

namespace gconf::shm::v1 {

struct alignas(64) PrivateEventEntry {
  std::atomic<std::uint64_t> seq{0}; // 8B 编码 = (slot << 1) | writing_bit; 0 = 未写过
  std::uint64_t client_order_id_digits;          // 8B clientOrderId 累加成 uint64
  std::uint64_t T_ns;                // 8B transaction time
  std::uint32_t filled_vol;          // 4B 累计成交数量 × 10^qty_scale
  std::uint32_t avg_price;           // 4B 成交均价 × 10^price_scale
  std::uint32_t last_price;          // 4B 末次成交价格 × 10^price_scale
  std::uint32_t last_vol;            // 4B 末次成交数量 × 10^qty_scale
  std::uint32_t E_t_ns_frac;         // 4B E - T (event time − transaction time)
  std::uint32_t shm_t_ns_frac;       // 4B shm write_ns - T_ns
  std::uint32_t kernel_t_ns_frac;    // 4B 内核 recv_ns - T_ns
  std::uint16_t local_symbol_id;          // 2B subset27 local id (local symbol id)
  std::uint16_t path_idx;            // 2B 写入此 entry 的 path 编号
  std::uint8_t status;               // 1B enums::OrderStatus
  std::uint8_t price_scale;          // 1B
  std::uint8_t qty_scale;            // 1B
  std::uint8_t account_id;           // 1B 账户编号
  std::uint32_t remote_ipv4_be;      // 4B 写入该 entry 的 WS 对端 IPv4 (network order)
                                     // Total 64B

  // seqlock 写入两段式. slot = producer fetch_add 得到的绝对 sequence; 编码进 seq,
  // consumer 据此识别本圈数据, 防 fetch_add → write_begin 空窗期误读上一圈遗留 even seq。
  void write_begin(std::uint64_t slot) noexcept { seq.store((slot << 1) | 1ULL, std::memory_order_release); } // 奇, 写中
  void write_end(std::uint64_t slot) noexcept { seq.store(slot << 1, std::memory_order_release); }            // 偶, 可读
};
static_assert(sizeof(PrivateEventEntry) == 64, "PrivateEventEntry must be 64B (1 cache line)");
static_assert(alignof(PrivateEventEntry) == 64, "PrivateEventEntry must be 64B aligned");

// 私有事件 ring 默认容量: 私有 TPS 通常 < 200, 峰值 < 1000; 1024 留足 backlog.
inline constexpr std::size_t PRIVATE_EVENT_RING_CAP = 1024;

// MPMC broadcast ring (also reused by order_event.h):
//   Producer: N 路 dispatcher 共享, dedup 后 fetch_add head 抢一格写入。
//   Consumer: M 个 (落盘 / 策略 / latency …), 各持本地 tail, load(head, acquire) 后扫。
template <typename Entry, std::size_t Cap> struct PrivateEventRing {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of 2");
  static constexpr std::size_t MASK = Cap - 1;

  alignas(64) std::atomic<std::uint64_t> head{0};  // producer fetch_add; slot & MASK = 下标
  alignas(64) Entry entries[Cap];
};

} // namespace gconf::shm::v1
