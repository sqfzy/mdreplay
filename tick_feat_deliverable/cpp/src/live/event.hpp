// event.hpp — 流式引擎与数据源解耦的事件契约
//
// 引擎只认这三种事件, 不认 RawTable/JSONL/WS/shm。replay 从 RawTable 行造事件,
// 未来 WsFeed/ShmFeed 产同样事件即可复用引擎。
//
// 价一律 ×1e8 的原始 int64、量一律真实值 double —— 与 RawTable / 离线 compute_day
// 的 `/PX_SCALE` 口径完全一致, 这样引擎内的 mid/imb/spread 走与离线同一条浮点路径。
#pragma once

#include <array>
#include <cstdint>

namespace tick_feat::live {

// OB 快照: 价 ×1e8 原始 int64; 前5档量真实值(引擎内 /PX_SCALE, 同离线 extract_ob_snapshots)
struct ObEvent {
    int64_t              ts;
    int64_t              bid_px0, ask_px0;
    std::array<double, 5> bid_sz, ask_sz;
};

// 成交: price ×1e8 原始; amount 真实币量; sign 由 side(0=主动买/1=主动卖)
struct TradeEvent {
    int64_t ts;
    int64_t side;
    int64_t price_scaled;
    double  amount;
};

// BN 行情(只取桶末 mid): 价 ×1e8 原始, 引擎按离线同表达式算 mid
struct BnMidEvent {
    int64_t ts;
    int64_t bid_px0, ask_px0;
};

} // namespace tick_feat::live
