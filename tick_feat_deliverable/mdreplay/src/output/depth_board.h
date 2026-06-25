#pragma once
// depth_board.h — mdreplay **本地自定义**多档行情段(gconf v1.2.2 无多档段,经用户决策本地定义)。
//
// 复用 gconf 的 SegHeader / seqlock / schema_fnv 框架原语(这些是通用 SHM 框架,不是段契约);
// DepthBoardSlot / DepthBoard 本身是 **mdreplay-local 契约**——下游须按本布局消费,非官方 gconf 段。
//
// 单一定长槽存 kMaxDepth(=25)档,`depth` 字段标有效档数(消费者据此知读几档);capacity = N_SYMS,
// 按 LID 索引 slot,claim_if_newer(update_id) 去重——与 BookTickBoard 同套语义,只是多档而非单档 BBO。
// depth==1 的 BBO 仍走 BookTickBoard(契约不变);depth>1 才落本段。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <gconf/symbol/symbol_idx.h>

#include "core/record.hpp"  // kMaxDepth

namespace mdreplay {

// 多档槽:seqlock 保护(偶可读/奇写入中);kMaxDepth 档定长存储,depth 标有效档数(1..kMaxDepth,0=最优)。
struct alignas(64) DepthBoardSlot {
  std::atomic<std::uint64_t> seq{0};   // seqlock:偶=可读,奇=写入中
  std::int64_t  exch_ns{0};            // 交易所时间戳(全精度 ns)
  std::uint64_t update_id{0};          // 交易所盘口更新序号(消费者据此感知跳变)
  std::uint16_t symbol_lid{0};         // LID
  std::uint8_t  price_scale{0}, qty_scale{0};  // 全档共用价/量定点指数
  std::uint8_t  depth{0};              // 有效档数(1..kMaxDepth);[0..depth) 才是真实档,其余为 0
  std::uint8_t  _rsvd[3]{};
  std::uint32_t bid_px[kMaxDepth]{}, bid_qty[kMaxDepth]{};  // 0=最优档,升序;定点 ×10^scale
  std::uint32_t ask_px[kMaxDepth]{}, ask_qty[kMaxDepth]{};

  // 生产者:seqlock 包住所有字段写入(偶 s → 奇 s+1 → payload → 偶 s+2)。全档定长拷贝(尾档为 0)。
  void write(std::int64_t t_ns, std::uint64_t upd, std::uint16_t lid, std::uint8_t p_scale,
             std::uint8_t q_scale, std::uint8_t d, const std::uint32_t* bpx, const std::uint32_t* bqty,
             const std::uint32_t* apx, const std::uint32_t* aqty) noexcept {
    namespace seqlock      = gconf::shm::v2::seqlock;
    const std::uint64_t s = seq.load(std::memory_order_relaxed);
    seqlock::begin(seq, s + 1);  // 奇 + 前沿 fence
    exch_ns = t_ns; update_id = upd; symbol_lid = lid;
    price_scale = p_scale; qty_scale = q_scale; depth = d;
    for (std::size_t k = 0; k < kMaxDepth; ++k) {
      bid_px[k] = bpx[k]; bid_qty[k] = bqty[k];
      ask_px[k] = apx[k]; ask_qty[k] = aqty[k];
    }
    seqlock::end(seq, s + 2);  // 偶 + 后沿 release
  }

  // 消费者:返回 false = 读到撕裂(写入中或期间被改),需重试。out.seq 不写(原子不拷贝)。
  [[nodiscard]] bool read(DepthBoardSlot& out) const noexcept {
    namespace seqlock       = gconf::shm::v2::seqlock;
    const std::uint64_t s1 = seqlock::snapshot(seq);
    if (s1 & 1) return false;  // 写入中
    out.exch_ns = exch_ns; out.update_id = update_id; out.symbol_lid = symbol_lid;
    out.price_scale = price_scale; out.qty_scale = qty_scale; out.depth = depth;
    for (std::size_t k = 0; k < kMaxDepth; ++k) {
      out.bid_px[k] = bid_px[k]; out.bid_qty[k] = bid_qty[k];
      out.ask_px[k] = ask_px[k]; out.ask_qty[k] = ask_qty[k];
    }
    return seqlock::verify(seq, s1);  // 期间无写者 ⇔ 一致
  }
};
static_assert(alignof(DepthBoardSlot) == 64, "DepthBoardSlot must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<DepthBoardSlot>, "DepthBoardSlot must be POD (SHM contract)");
static_assert(offsetof(DepthBoardSlot, seq) == 0, "seq must lead the slot (seqlock)");
static_assert(offsetof(DepthBoardSlot, exch_ns) == 8, "exch_ns @ +8");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "SHM seqlock atomic must be lock-free");

// schema_hash:本段字段布局的编译期 FNV(含档数 N=kMaxDepth);改布局即改串 → attach 告警。
inline constexpr std::uint64_t kDepthBoardSchemaHash = gconf::shm::v2::schema_fnv(
    "DepthBoardSlot:seq8,exch_ns8,update_id8,lid2,price_scale1,qty_scale1,depth1,rsvd3,"
    "bid_px4xN,bid_qty4xN,ask_px4xN,ask_qty4xN;N=25");

// 多档行情板:段头 + per-LID best_uid(CAS 去重)+ per-LID seqlock 槽。capacity = N_SYMS。
struct DepthBoard {
  gconf::shm::v2::SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> best_uid[gconf::sym::N_SYMS]{};  // CAS 去重(竞速赢家)
  alignas(64) DepthBoardSlot slot[gconf::sym::N_SYMS];

  // 与 BookTickBoard 同语义:仅当 update_id 更新时赢得本槽(CAS)。true ⇒ 本路赢,可写 slot。
  [[nodiscard]] bool claim_if_newer(int lid, std::uint64_t update_id, std::uint64_t& replaced_old) noexcept {
    std::uint64_t prev = best_uid[lid].load(std::memory_order_relaxed);
    while (update_id > prev)
      if (best_uid[lid].compare_exchange_weak(prev, update_id, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
        replaced_old = prev;
        return true;
      }
    return false;
  }
};
static_assert(std::is_standard_layout_v<DepthBoard>, "DepthBoard must be standard-layout (SHM contract)");

}  // namespace mdreplay
