#pragma once
// gconf/shm/v2/depth_board.h — v2 五档行情板(latest-wins,per-global symbol id seqlock 槽)。
//
// 与 board.h 的 BBO Board 同范式(SegKind::Board、best_uid CAS 去重、seqlock 槽),唯一区别是
// 每槽携带 5 档 bid/ask 价量而非 1 档。靠 entry_size(sizeof(DepthSlot))+ 独立 schema_hash 与
// BBO Board 区分,attach 时 seg_check 自校验。给需要盘口深度(如 imb5)的下游用。
//
// 多档共用单一 price_scale/qty_scale(对齐 mdreplay Record:全档价一组、全档量一组编码)。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <gconf/domain/symbols.h>

namespace gconf::shm::v2 {

inline constexpr std::size_t kDepthLevels = 5;  // 档数(POD 段编译期定长)

// 单 symbol 的五档行情槽:2 cache line(128B),seqlock 保护。档按升序,[0]=最优档。
struct alignas(64) DepthSlot {
  std::atomic<std::uint64_t> seq{0};  // seqlock:偶=可读,奇=写入中
  std::int64_t  exch_ns{0};           // 交易所交易时间戳(全精度 ns)
  std::uint64_t update_id{0};         // 交易所 update id(消费者据此感知跳变)
  std::uint32_t bid_px[kDepthLevels]{}, bid_qty[kDepthLevels]{};  // value×10^scale,[0]=最优
  std::uint32_t ask_px[kDepthLevels]{}, ask_qty[kDepthLevels]{};
  std::uint16_t global_symbol_id{0};
  std::uint8_t  price_scale{0}, qty_scale{0};
  std::uint8_t  depth{0};             // 有效档数(=kDepthLevels;留字段便于校验/演进)
  std::uint8_t  _rsvd[19]{};          // 凑齐 128B

  // 生产者:seqlock 包住全部字段写入。
  void write(std::int64_t t_ns, std::uint64_t upd, const std::uint32_t* bp, const std::uint32_t* bq,
             const std::uint32_t* ap, const std::uint32_t* aq, std::uint16_t sym,
             std::uint8_t p_scale, std::uint8_t q_scale, std::uint8_t d) noexcept {
    const std::uint64_t s = seq.load(std::memory_order_relaxed);
    seqlock::begin(seq, s + 1);
    exch_ns = t_ns; update_id = upd;
    for (std::size_t k = 0; k < kDepthLevels; ++k) {
      bid_px[k] = bp[k]; bid_qty[k] = bq[k]; ask_px[k] = ap[k]; ask_qty[k] = aq[k];
    }
    global_symbol_id = sym; price_scale = p_scale; qty_scale = q_scale; depth = d;
    seqlock::end(seq, s + 2);
  }

  // 消费者:返回 false = 读到撕裂(写入中或期间被改),需重试。
  [[nodiscard]] bool read(DepthSlot& out) const noexcept {
    const std::uint64_t s1 = seqlock::snapshot(seq);
    if (s1 & 1) return false;
    out.exch_ns = exch_ns; out.update_id = update_id;
    for (std::size_t k = 0; k < kDepthLevels; ++k) {
      out.bid_px[k] = bid_px[k]; out.bid_qty[k] = bid_qty[k];
      out.ask_px[k] = ask_px[k]; out.ask_qty[k] = ask_qty[k];
    }
    out.global_symbol_id = global_symbol_id; out.price_scale = price_scale;
    out.qty_scale = qty_scale; out.depth = depth;
    return seqlock::verify(seq, s1);
  }
};
static_assert(sizeof(DepthSlot) == 128, "DepthSlot must be 2 cache lines");
static_assert(alignof(DepthSlot) == 64, "DepthSlot must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<DepthSlot>, "DepthSlot must be POD (SHM contract)");
static_assert(offsetof(DepthSlot, seq) == 0, "seq must lead the slot (seqlock)");
static_assert(offsetof(DepthSlot, exch_ns) == 8 && offsetof(DepthSlot, update_id) == 16 &&
              offsetof(DepthSlot, bid_px) == 24,
              "DepthSlot id/BBO field offsets are the SHM contract — lock them");

inline constexpr std::uint64_t kDepthBoardSchemaHash = schema_fnv(
    "DepthSlot:seq8,exch_ns8,update_id8,bid_px20,bid_qty20,ask_px20,ask_qty20,"
    "gid2,price_scale1,qty_scale1,depth1");

// 五档行情板:段头 + per-global symbol id best_uid(CAS 去重)+ per-global symbol id seqlock 槽。
struct DepthBoard {
  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> best_uid[sym::N_GLOBAL_SYMBOL_IDS]{};
  alignas(64) DepthSlot slot[sym::N_GLOBAL_SYMBOL_IDS];

  [[nodiscard]] bool claim_if_newer(int global_symbol_id, std::uint64_t update_id) noexcept {
    std::uint64_t prev = best_uid[global_symbol_id].load(std::memory_order_relaxed);
    while (update_id > prev)
      if (best_uid[global_symbol_id].compare_exchange_weak(prev, update_id, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed))
        return true;
    return false;
  }
};
static_assert(std::is_standard_layout_v<DepthBoard>, "DepthBoard must be standard-layout (SHM contract)");

}  // namespace gconf::shm::v2
