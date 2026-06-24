#pragma once
#include <atomic>
#include <cstdint>

#include <gconf/domain/symbols.h>

// Top-of-book (BBO) SHM contract — bn-standard ShmSlot + per-symbol snap board.
//
// ShmSlot: 64B seqlock slot (1 cache line). Prices/qtys are FIXED-POINT integers
// (value × 10^scale); the consumer restores decimals via price_scale/qty_scale.
//   write: seq → s+1 (odd) → payload → s+2 (even)
//   read : even seq + head==tail = consistent snapshot
//
// ShmMarketSnapBoard: per-symbol best_uid (CAS dedup) + per-symbol slot (seqlock),
// indexed by global symbol id (covers the full universe incl BTC/ETH).

namespace gconf::shm::v1 {

struct alignas(64) ShmSlot {
  std::atomic<uint64_t> seq{0};      // 8B  seqlock 序号 (奇=写入中, 偶=可读)
  uint64_t u_id{0};                  // 8B  交易所 update id
  uint64_t T_ns{0};                  // 8B  交易时间戳 ns (transaction time)
  std::uint32_t bid_price{0};        // 4B  买价 × 10^price_scale
  std::uint32_t bid_num{0};          // 4B  买量 × 10^qty_scale
  std::uint32_t ask_price{0};        // 4B  卖价 × 10^price_scale
  std::uint32_t ask_num{0};          // 4B  卖量 × 10^qty_scale
  std::uint32_t kernel_T_ns_frac{0}; // 4B  kernel_recv_ns - T_ns
  std::uint32_t write_T_ns_frac{0};  // 4B  write_ns - T_ns (producer flush 延迟)
  std::uint32_t recv_T_ns_frac{0};   // 4B  on_message recv_ns - T_ns
  std::uint16_t symid{0};            // 2B  global symbol id (global symbol id)
  std::uint16_t path_idx{0};         // 2B  竞速路径编号 (哪条路赢了 CAS)
  std::uint8_t price_scale{0};       // 1B  consumer 还原小数用
  std::uint8_t qty_scale{0};         // 1B

  // 生产者写: seqlock 包住所有字段
  void write(uint64_t update_id, uint64_t t_ns, std::uint32_t bid_p, std::uint32_t bid_n, std::uint32_t ask_p, std::uint32_t ask_n, std::uint16_t sym,
             std::uint8_t p_scale, std::uint8_t q_scale, std::uint32_t kernel_t_ns_frac, std::uint32_t write_t_ns_frac, std::uint32_t recv_t_ns_frac,
             std::uint16_t path) noexcept {
    uint64_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_release);
    u_id = update_id;
    T_ns = t_ns;
    bid_price = bid_p;
    bid_num = bid_n;
    ask_price = ask_p;
    ask_num = ask_n;
    kernel_T_ns_frac = kernel_t_ns_frac;
    write_T_ns_frac = write_t_ns_frac;
    recv_T_ns_frac = recv_t_ns_frac;
    symid = sym;
    path_idx = path;
    price_scale = p_scale;
    qty_scale = q_scale;
    seq.store(s + 2, std::memory_order_release);
  }

  // 消费者读 (返回 false = 读到撕裂数据, 需重试)
  bool read(ShmSlot &out) const noexcept {
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1)
      return false;
    out.u_id = u_id;
    out.T_ns = T_ns;
    out.bid_price = bid_price;
    out.bid_num = bid_num;
    out.ask_price = ask_price;
    out.ask_num = ask_num;
    out.kernel_T_ns_frac = kernel_T_ns_frac;
    out.write_T_ns_frac = write_T_ns_frac;
    out.recv_T_ns_frac = recv_T_ns_frac;
    out.symid = symid;
    out.path_idx = path_idx;
    out.price_scale = price_scale;
    out.qty_scale = qty_scale;
    uint64_t s2 = seq.load(std::memory_order_acquire);
    return s1 == s2;
  }
};
static_assert(sizeof(ShmSlot) == 64, "ShmSlot expected 64B (single cache line)");
static_assert(alignof(ShmSlot) == 64, "ShmSlot must be 64B aligned");

template <typename Slot, int MaxSymbols> struct ShmMarketSnapBoard {
  // CAS 去重：每个 symbol 一个 atomic uid（交易所序列号）。写者多读者少，CAS 比 SeqLock 省跨核 invalidation。
  alignas(64) std::atomic<std::uint64_t> best_uid[MaxSymbols]{};
  // 赢家数据：每个 symbol 一个 slot（SeqLock 保护，下游读者无法加锁靠 seq 检测撕裂）。
  alignas(64) Slot board[MaxSymbols];
};

// bybit 行情板：global symbol id 索引，覆盖全宇宙 (N_GLOBAL_SYMBOL_IDS)。
using BookTickBoard = ShmMarketSnapBoard<ShmSlot, sym::N_GLOBAL_SYMBOL_IDS>;

} // namespace gconf::shm::v1
