#pragma once

// POSIX SHM object names — part of the contract (producer & consumer must open
// the SAME name). Names stay venue-specific (they ARE distinct SHM objects);
// only the bybit-linear set is kept here.

namespace gconf::shm::v1 {

inline constexpr const char *SHM_BYBIT_LIN_BOOK_TICK         = "/shm_bybit_lin_book_tick";
inline constexpr const char *SHM_BYBIT_LIN_SYMINFO          = "/shm_bybit_lin_syminfo";
inline constexpr const char *SHM_BYBIT_LIN_PRIVATE_EVENT_RING = "/shm_bybit_lin_private_event_ring";
inline constexpr const char *SHM_BYBIT_LIN_ORDER_EVENT_RING   = "/shm_bybit_lin_order_event_ring";

// 策略 → 交易引擎请求 ring (全交易所共享一条, 按 frame.ex/ac 路由)。
inline constexpr const char *SHM_PRIVATE_REQ_RING = "/shm_private_req_ring";

} // namespace gconf::shm::v1
