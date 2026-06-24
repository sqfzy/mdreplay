#pragma once
// gconf/shm/v2/names.h — v2 SHM 段名(bybit-linear set)。
//
// 段名是 ABI 契约(producer/consumer 必须 open 同名)。v1/v2 段并存以支持双写灰度,
// 故内核对象名里带版本后缀(/shm_bybit_lin_<kind>_v2)。
//
// 标识符本身【不带版本】(BOOK/ORDER_EVT/...):消费侧 `namespace cur = gconf::shm::v2;`
// 后用 cur::BOOK;升 v3 只改这一行别名,标识符与调用点不动(段名字面量随命名空间整体替换)。
//
// 红线:这里只放 bybit-linear 名;binance-spot 在 binance/shm_names.hpp 自带(不在 gconf 加别的 venue)。

namespace gconf::shm::v2 {

inline constexpr const char* BOOK      = "/shm_bybit_lin_book_v2";
inline constexpr const char* TRADE     = "/shm_bybit_lin_trade_v2";      // taker trade stream (BcastRing)
inline constexpr const char* ORDER_EVT = "/shm_bybit_lin_order_evt_v2";  // order ack ring (BcastRing)
inline constexpr const char* PRIV_EVT  = "/shm_bybit_lin_priv_evt_v2";   // private fill/lifecycle ring (BcastRing)
inline constexpr const char* REQ       = "/shm_bybit_lin_req_v2";        // per-venue req SpscQueue

}  // namespace gconf::shm::v2
