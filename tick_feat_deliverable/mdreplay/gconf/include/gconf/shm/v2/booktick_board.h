#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gconf/symbol/symbol_idx.h>
#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <type_traits>

namespace gconf::shm::v2 {

// 单 symbol 的行情槽:64B,一 cache line,seqlock 保护。
struct alignas(64) BookTickBoardSlot {
  std::atomic<std::uint64_t> seq{0};                          // seqlock:偶=可读,奇=写入中(v2/seqlock.h)
  std::int64_t exch_ns{0};                                    // 交易所交易时间戳(全精度 ns)
  std::uint64_t update_id{0};                                 // 交易所 update id(消费者据此感知跳变)
  std::uint32_t bid_px{0}, bid_qty{0}, ask_px{0}, ask_qty{0}; // 盘口信息
  std::uint32_t kernel_T_ns_frac{0};                          // kernel_recv_ns 减 exch_ns
  std::uint32_t shm_T_ns_frac{0};                             // write_ns 减 exch_ns (producer flush 相对 transaction time 的延迟)
  std::uint32_t E_T_ns_frac{0};                               //
  std::uint16_t symbol_lid{0};                                // LID
  std::uint8_t price_scale{0}, qty_scale{0};                  // 价格精度和数量精度
  std::uint8_t path_idx{0};                                   // 竞速赢家路径编号
  std::uint8_t _rsvd[7]{};

  // 生产者:seqlock 包住所有字段写入(偶 s → 奇 s+1 → payload → 偶 s+2)。
  void write(std::int64_t t_ns, std::uint64_t upd, std::uint32_t bid_p, std::uint32_t bid_q, std::uint32_t ask_p, std::uint32_t ask_q,
             std::uint32_t kernel_t_ns_frac, std::uint32_t shm_t_ns_frac, std::uint32_t E_t_ns_frac, std::uint16_t sym, std::uint8_t p_scale,
             std::uint8_t q_scale, std::uint8_t path) noexcept {
    const std::uint64_t s = seq.load(std::memory_order_relaxed);
    seqlock::begin(seq, s + 1); // 奇 + 前沿 fence
    exch_ns = t_ns;
    update_id = upd;
    bid_px = bid_p;
    bid_qty = bid_q;
    ask_px = ask_p;
    ask_qty = ask_q;
    kernel_T_ns_frac = kernel_t_ns_frac;
    shm_T_ns_frac = shm_t_ns_frac;
    E_T_ns_frac = E_t_ns_frac;
    symbol_lid = sym;
    price_scale = p_scale;
    qty_scale = q_scale;
    path_idx = path;
    seqlock::end(seq, s + 2); // 偶 + 后沿 release
  }

  // 消费者:返回 false = 读到撕裂(写入中或期间被改),需重试。
  [[nodiscard]] bool read(BookTickBoardSlot &out) const noexcept {
    const std::uint64_t s1 = seqlock::snapshot(seq);
    if (s1 & 1)
      return false; // 写入中
    out.exch_ns = exch_ns;
    out.update_id = update_id;
    out.bid_px = bid_px;
    out.bid_qty = bid_qty;
    out.ask_px = ask_px;
    out.ask_qty = ask_qty;
    out.kernel_T_ns_frac = kernel_T_ns_frac;
    out.shm_T_ns_frac = shm_T_ns_frac;
    out.E_T_ns_frac = E_T_ns_frac;
    out.symbol_lid = symbol_lid;
    out.price_scale = price_scale;
    out.qty_scale = qty_scale;
    out.path_idx = path_idx;
    return seqlock::verify(seq, s1); // 期间无写者 ⇔ 一致
  }
};
static_assert(sizeof(BookTickBoardSlot) == 64, "BookTickBoardSlot must be exactly one cache line");
static_assert(alignof(BookTickBoardSlot) == 64, "BookTickBoardSlot must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<BookTickBoardSlot>, "BookTickBoardSlot must be POD (SHM contract)");
static_assert(offsetof(BookTickBoardSlot, seq) == 0, "seq must lead the slot (seqlock)");
static_assert(offsetof(BookTickBoardSlot, exch_ns) == 8, "exch_ns @ +8");
// 锁定 id / BBO 字段偏移:跨进程 padding 漂移即便 sizeof 相等也会让 BBO 错位 → 据错价下单(#82)。
static_assert(offsetof(BookTickBoardSlot, update_id) == 16 && offsetof(BookTickBoardSlot, bid_px) == 24 && offsetof(BookTickBoardSlot, ask_px) == 32
                  && offsetof(BookTickBoardSlot, symbol_lid) == 52,
              "BookTickBoardSlot id/BBO field offsets are the SHM contract — lock them");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "SHM seqlock atomic must be lock-free — 跨进程同步前提(#90)");

inline constexpr std::uint64_t kBoardSchemaHash = schema_fnv("BookTickBoardSlot:seq8,exch_ns8,update_id8,bid_px4,bid_qty4,ask_px4,ask_qty4,"
                                                             "kernel_T4,shm_T4,E_T4,lid2,price_scale1,qty_scale1,path_idx1");

// 行情板:段头 + per-LID best_uid(CAS 去重)+ per-LID seqlock 槽。capacity = N_SYMS。
struct BookTickBoard {
  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> best_uid[sym::N_SYMS]{}; // CAS 去重(竞速赢家)
  alignas(64) BookTickBoardSlot slot[sym::N_SYMS];

  // 多写者竞速去重:仅当 update_id 比已记录的新时赢得本槽(CAS)。true ⇒ 本路是赢家,可写 slot。
  //
  // 调用方协议(契约侧无法强制,#92):同一 lid 的 slot[lid].write() 必须由 claim_if_newer 的 CAS
  // 赢家串行化——即"先 claim 赢、再 write"。两 path 同时进 write 同 slot 会让 seqlock 奇/偶序交织,
  // 消费者读到奇 seq 或撕裂 BBO。生产端单写者(单 worker)天然满足;多 worker 必须靠本 CAS 串行化。
  // replaced_old(出参): CAS 赢时回填被替换掉的旧 uid, 调用方免去再做一次 best_uid[lid].load。
  // 返回 false 时不写 replaced_old(调用方仅在 true 分支读它)。
  [[nodiscard]] bool claim_if_newer(int lid, std::uint64_t update_id, std::uint64_t &replaced_old) noexcept {
    std::uint64_t prev = best_uid[lid].load(std::memory_order_relaxed);
    while (update_id > prev)
      if (best_uid[lid].compare_exchange_weak(prev, update_id, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        replaced_old = prev; // CAS 成功时 prev 未被改写, 即被替换的旧值
        return true;
      }
    return false;
  }
};
static_assert(std::is_standard_layout_v<BookTickBoard>, "Board must be standard-layout (SHM contract)");

} // namespace gconf::shm::v2
