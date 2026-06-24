#pragma once
// gconf/shm/v2/board.h — v2 行情板(latest-wins,per-global symbol id seqlock 槽)。
//
// 相对 v1 BookTickBoard 的改进:
//   - seqlock 用 v2/seqlock.h 的正确前后沿 fence(修 #10,aarch64 无撕裂)。
//   - 段头自描述(SegHeader)+ 编译期 schema_hash。
//   - 时间戳全精度:exch_ns(int64)+ 三个有符号延迟(int32,相对 exch_ns)——废弃 v1 uint32 frac
//     增量(>4.29s 溢出的过早优化)。
//   - 结构沿用 v1:per-global symbol id best_uid(CAS 去重,多写者少读者省跨核 invalidation)+ slot(seqlock)。
//
// latest-wins:只保留最新 BBO,丢旧的本来就对;消费者用 update_id 跳变感知"跳过了 N 个中间 tick"。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>
#include <gconf/domain/symbols.h>

namespace gconf::shm::v2 {

// 单 symbol 的行情槽:64B,一 cache line,seqlock 保护。
struct alignas(64) BoardSlot {
  std::atomic<std::uint64_t> seq{0};  // seqlock:偶=可读,奇=写入中(v2/seqlock.h)
  std::int64_t  exch_ns{0};           // 交易所交易时间戳(全精度 ns)
  std::uint64_t update_id{0};              // 交易所 update id(消费者据此感知跳变)
  std::uint32_t bid_px{0}, bid_qty{0}, ask_px{0}, ask_qty{0};  // value×10^scale;u32 上限~4.29e9,要求 value×10^scale 不溢出(当前 subset 价×scale≤~1e6,余量足;扩高价 symbol 前须复核 scale,#81)
  std::int32_t  kernel_lat_ns{0}, recv_lat_ns{0}, write_lat_ns{0};  // 相对 exch_ns 的有符号延迟
  std::uint16_t global_symbol_id{0};             // global symbol id
  std::uint8_t  price_scale{0}, qty_scale{0};
  std::uint8_t  path_idx{0};          // 竞速赢家路径编号
  std::uint8_t  _rsvd[7]{};

  // 生产者:seqlock 包住所有字段写入(偶 s → 奇 s+1 → payload → 偶 s+2)。
  void write(std::int64_t t_ns, std::uint64_t upd,
             std::uint32_t bid_p, std::uint32_t bid_q, std::uint32_t ask_p, std::uint32_t ask_q,
             std::int32_t kernel_lat, std::int32_t recv_lat, std::int32_t write_lat,
             std::uint16_t sym, std::uint8_t p_scale, std::uint8_t q_scale, std::uint8_t path) noexcept {
    const std::uint64_t s = seq.load(std::memory_order_relaxed);
    seqlock::begin(seq, s + 1);                       // 奇 + 前沿 fence
    exch_ns = t_ns; update_id = upd;
    bid_px = bid_p; bid_qty = bid_q; ask_px = ask_p; ask_qty = ask_q;
    kernel_lat_ns = kernel_lat; recv_lat_ns = recv_lat; write_lat_ns = write_lat;
    global_symbol_id = sym; price_scale = p_scale; qty_scale = q_scale; path_idx = path;
    seqlock::end(seq, s + 2);                          // 偶 + 后沿 release
  }

  // 消费者:返回 false = 读到撕裂(写入中或期间被改),需重试。
  [[nodiscard]] bool read(BoardSlot& out) const noexcept {
    const std::uint64_t s1 = seqlock::snapshot(seq);
    if (s1 & 1) return false;                          // 写入中
    out.exch_ns = exch_ns; out.update_id = update_id;
    out.bid_px = bid_px; out.bid_qty = bid_qty; out.ask_px = ask_px; out.ask_qty = ask_qty;
    out.kernel_lat_ns = kernel_lat_ns; out.recv_lat_ns = recv_lat_ns; out.write_lat_ns = write_lat_ns;
    out.global_symbol_id = global_symbol_id; out.price_scale = price_scale; out.qty_scale = qty_scale; out.path_idx = path_idx;
    return seqlock::verify(seq, s1);                   // 期间无写者 ⇔ 一致
  }
};
static_assert(sizeof(BoardSlot) == 64, "BoardSlot must be exactly one cache line");
static_assert(alignof(BoardSlot) == 64, "BoardSlot must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<BoardSlot>, "BoardSlot must be POD (SHM contract)");
static_assert(offsetof(BoardSlot, seq) == 0, "seq must lead the slot (seqlock)");
static_assert(offsetof(BoardSlot, exch_ns) == 8, "exch_ns @ +8");
// 锁定 id / BBO 字段偏移:跨进程 padding 漂移即便 sizeof 相等也会让 BBO 错位 → 据错价下单(#82)。
static_assert(offsetof(BoardSlot, update_id) == 16 && offsetof(BoardSlot, bid_px) == 24 &&
              offsetof(BoardSlot, ask_px) == 32 && offsetof(BoardSlot, global_symbol_id) == 52,
              "BoardSlot id/BBO field offsets are the SHM contract — lock them");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SHM seqlock atomic must be lock-free — 跨进程同步前提(#90)");

inline constexpr std::uint64_t kBoardSchemaHash = schema_fnv(
    "BoardSlot:seq8,exch_ns8,update_id8,bid_px4,bid_qty4,ask_px4,ask_qty4,"
    "kernel_lat4,recv_lat4,write_lat4,gid2,price_scale1,qty_scale1,path_idx1");

// 行情板:段头 + per-global symbol id best_uid(CAS 去重)+ per-global symbol id seqlock 槽。capacity = N_GLOBAL_SYMBOL_IDS。
struct Board {
  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> best_uid[sym::N_GLOBAL_SYMBOL_IDS]{};  // CAS 去重(竞速赢家)
  alignas(64) BoardSlot slot[sym::N_GLOBAL_SYMBOL_IDS];

  // 多写者竞速去重:仅当 update_id 比已记录的新时赢得本槽(CAS)。true ⇒ 本路是赢家,可写 slot。
  //
  // 调用方协议(契约侧无法强制,#92):同一 global_symbol_id 的 slot[global_symbol_id].write() 必须由 claim_if_newer 的 CAS
  // 赢家串行化——即"先 claim 赢、再 write"。两 path 同时进 write 同 slot 会让 seqlock 奇/偶序交织,
  // 消费者读到奇 seq 或撕裂 BBO。生产端单写者(单 worker)天然满足;多 worker 必须靠本 CAS 串行化。
  [[nodiscard]] bool claim_if_newer(int global_symbol_id, std::uint64_t update_id) noexcept {
    std::uint64_t prev = best_uid[global_symbol_id].load(std::memory_order_relaxed);
    while (update_id > prev)
      if (best_uid[global_symbol_id].compare_exchange_weak(prev, update_id, std::memory_order_acq_rel, std::memory_order_relaxed))
        return true;
    return false;
  }
};
static_assert(std::is_standard_layout_v<Board>, "Board must be standard-layout (SHM contract)");

}  // namespace gconf::shm::v2
