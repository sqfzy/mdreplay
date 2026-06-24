#pragma once
// gconf/shm/v2/event.h — v2 事件广播环(order ack / private fill/lifecycle 统一)。
//
// 范式:Vyukov bounded MPMC(per-cell slot-encoded seq)+ Disruptor。生产者多路(N dispatcher)
// fetch_add 抢绝对槽 slot,在 entry[slot&MASK] 上 seqlock 写,seq 编码 (slot<<1)|writing,消费者据此
// 识别本圈数据并【检测 lapped】(head-tail>Cap = 被绕圈,丢了 head-tail-Cap 条 → 返回丢失计数)。
//
// 统一 EventEntry(ev_kind 区分 ack/fill/lifecycle),order/private 各用一段(同布局,不同段)。
// 不丢成交靠 resync(消费者拿到丢失计数后从 REST snapshot 补,D1),不是无限加大环。
//
// 同步:EventEntry = { atomic seq(seqlock) ; EventPayload p(纯 POD,可拷贝) }。读者拷贝 p(不碰
// atomic),用 seqlock::verify 确认一致。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/seqlock.h>

namespace gconf::shm::v2 {

enum class EvKind : std::uint8_t { Ack = 1, Fill = 2, Lifecycle = 3, Count };

// 事件 payload(纯 POD,可拷贝;不含 atomic)。56B,使 EventEntry 凑齐 64B。
struct EventPayload {
  std::int64_t  exch_ns{0};      // 交易所时间戳(全精度 ns)
  std::uint64_t client_order_id{0};          // client_order_id digits(跨流关联键)
  std::uint32_t filled_qty{0}, avg_px{0}, last_px{0}, last_qty{0};  // ×10^scale
  std::uint16_t local_symbol_id{0};        // subset27 local symbol id(事件热路径索引,非 global symbol id;来自 req_local_symbol_id)
  EvKind        ev_kind{};       // ack / fill / lifecycle
  std::uint8_t  status{0};       // OrderStatus
  std::uint32_t err_code{0};     // u32:容纳 bybit retCode(110xxx/170xxx>u16);0xFFFFFFFF=传输故障哨兵(#84)
  std::uint8_t  resp_type{0};
  std::uint8_t  path_idx{0};
  std::uint8_t  price_scale{0}, qty_scale{0};
  std::uint8_t  _rsvd[12]{};
};
static_assert(sizeof(EventPayload) == 56, "EventPayload must be 56B (EventEntry → 64B)");
static_assert(std::is_trivially_copyable_v<EventPayload>, "EventPayload must be POD");
// 字段偏移即 SHM 契约:两进程 padding/编译器即便 sizeof 相等也可能偏移不同 → 逐字段错位静默错乱。
// 锁定关键字段(跨流关联键 client_order_id、资金字段、err_code)的偏移(#82/#84)。
static_assert(offsetof(EventPayload, exch_ns) == 0 && offsetof(EventPayload, client_order_id) == 8 &&
              offsetof(EventPayload, filled_qty) == 16 && offsetof(EventPayload, local_symbol_id) == 32 &&
              offsetof(EventPayload, err_code) == 36,
              "EventPayload field offsets are the SHM contract — lock them");

struct alignas(64) EventEntry {
  std::atomic<std::uint64_t> seq{0};  // seqlock:(slot<<1)|writing;0=从未写
  EventPayload               p;
};
static_assert(sizeof(EventEntry) == 64, "EventEntry must be one cache line");
static_assert(alignof(EventEntry) == 64, "EventEntry must be cache-line aligned");
static_assert(offsetof(EventEntry, seq) == 0 && offsetof(EventEntry, p) == 8,
              "EventEntry seq must lead (seqlock 圈号编码);payload @8 (#82)");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SHM seq/head atomic must be lock-free — 跨进程同步前提(#90)");

inline constexpr std::uint64_t kEventSchemaHash = schema_fnv(
    "EventEntry:seq8|exch_ns8,cid8,filled4,avg4,lastpx4,lastqty4,lid2,ev_kind1,"
    "status1,err4,resp1,path1,pscale1,qscale1");

inline constexpr std::size_t EVENT_RING_CAP = 1024;  // 2 的幂

// 广播环:段头 + producer head(独占 cache line)+ entries。消费者 tail 在 SHM 外(广播,不背压)。
template <class Entry, std::size_t Cap>
struct BcastRing {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");
  static constexpr std::size_t MASK = Cap - 1;
  using Payload = decltype(Entry::p);

  SegHeader hdr;
  alignas(64) std::atomic<std::uint64_t> head{0};   // producer fetch_add(独占行,防 false sharing)
  alignas(64) Entry entries[Cap];

  // 生产者:抢槽 + seqlock 写 payload。
  void publish(const Payload& pl) noexcept {
    const std::uint64_t slot = head.fetch_add(1, std::memory_order_relaxed);
    Entry& e = entries[slot & MASK];
    seqlock::begin(e.seq, (slot << 1) | 1);          // 奇(slot 编码)+ 前沿 fence
    e.p = pl;
    seqlock::end(e.seq, slot << 1);                   // 偶(slot 编码)+ 后沿 release
  }

  // 消费者:扫 [tail..head) 派发 fn(const Payload&)。返回【被绕圈丢失】的条数(>0 ⇒ 该 resync)。
  // 未就绪槽(写入中/MPMC 乱序未补齐)→ break 等下一拍,不静默跳过(不丢)。
  template <class Fn>
  std::uint64_t drain(std::uint64_t& tail, Fn&& fn) noexcept {
    const std::uint64_t h = head.load(std::memory_order_acquire);
    std::uint64_t lapped = 0;
    if (h - tail > Cap) { lapped = (h - tail) - Cap; tail = h - Cap; }   // 被绕圈:tail 跳到最近可读
    for (; tail < h; ++tail) {
      const Entry& e = entries[tail & MASK];
      const std::uint64_t s1 = seqlock::snapshot(e.seq);
      if (s1 != (tail << 1)) break;                  // 本槽尚未写好(写入中/还没轮到)→ 等
      Payload pl = e.p;                               // 拷贝 payload(不碰 atomic)
      if (!seqlock::verify(e.seq, s1)) break;         // 期间被写者改 → 重读
      fn(pl);
    }
    return lapped;
  }
};
static_assert(std::is_standard_layout_v<BcastRing<EventEntry, EVENT_RING_CAP>>,
              "BcastRing must be standard-layout (SHM contract)");

using EventRing = BcastRing<EventEntry, EVENT_RING_CAP>;

}  // namespace gconf::shm::v2
