#pragma once
#include <cstdint>

// Venue-neutral enums shared across the contract.
// (Reorganized from gconf private_/exchange — unified, no venue split.)

namespace gconf::enums {

// ── exchange / account routing (PrivateReqFrame.ex / .ac) ───────────────────
enum class ExType : std::uint8_t { BINANCE = 0, OKX, BYBIT, Count };

enum class AcType : std::uint8_t {
  SWAP = 0,
  SPOT,
  FULL_MARGIN,
  Count,   // 终结项:供消费端编译期统计/范围校验(SHM 内 ac 可能来自旧版/异常 producer)
};

// ── order direction / type (PlaceOrderReq.order_mode) ───────────────────────
enum class OrderMode : std::uint8_t {
  BUY_OPEN_LIMIT = 0,
  SELL_OPEN_LIMIT = 1,
  BUY_CLOSE_LIMIT = 2,
  SELL_CLOSE_LIMIT = 3,
  BUY_OPEN_LIMIT_MAKER = 4,
  SELL_OPEN_LIMIT_MAKER = 5,
  BUY_CLOSE_LIMIT_MAKER = 6,
  SELL_CLOSE_LIMIT_MAKER = 7,
  BUY_OPEN_MARKET = 8,
  BUY_CLOSE_MARKET = 9,
  SELL_OPEN_MARKET = 10,
  SELL_CLOSE_MARKET = 11,
  // 单向持仓 (one-way): positionSide 固定 BOTH, 与上方双向 12 个一一对应
  BUY_OPEN_LIMIT_BOTH = 12,
  SELL_OPEN_LIMIT_BOTH = 13,
  BUY_CLOSE_LIMIT_BOTH = 14,
  SELL_CLOSE_LIMIT_BOTH = 15,
  BUY_OPEN_LIMIT_MAKER_BOTH = 16,
  SELL_OPEN_LIMIT_MAKER_BOTH = 17,
  BUY_CLOSE_LIMIT_MAKER_BOTH = 18,
  SELL_CLOSE_LIMIT_MAKER_BOTH = 19,
  BUY_OPEN_MARKET_BOTH = 20,
  BUY_CLOSE_MARKET_BOTH = 21,
  SELL_OPEN_MARKET_BOTH = 22,
  SELL_CLOSE_MARKET_BOTH = 23,
  Count
};

[[nodiscard]] constexpr const char *to_string(OrderMode m) noexcept {
  constexpr const char *kNames[24] = {
      "BUY_OPEN_LIMIT",            "SELL_OPEN_LIMIT",       "BUY_CLOSE_LIMIT",
      "SELL_CLOSE_LIMIT",          "BUY_OPEN_LIMIT_MAKER",  "SELL_OPEN_LIMIT_MAKER",
      "BUY_CLOSE_LIMIT_MAKER",     "SELL_CLOSE_LIMIT_MAKER","BUY_OPEN_MARKET",
      "BUY_CLOSE_MARKET",          "SELL_OPEN_MARKET",      "SELL_CLOSE_MARKET",
      "BUY_OPEN_LIMIT_BOTH",       "SELL_OPEN_LIMIT_BOTH",  "BUY_CLOSE_LIMIT_BOTH",
      "SELL_CLOSE_LIMIT_BOTH",     "BUY_OPEN_LIMIT_MAKER_BOTH", "SELL_OPEN_LIMIT_MAKER_BOTH",
      "BUY_CLOSE_LIMIT_MAKER_BOTH","SELL_CLOSE_LIMIT_MAKER_BOTH","BUY_OPEN_MARKET_BOTH",
      "BUY_CLOSE_MARKET_BOTH",     "SELL_OPEN_MARKET_BOTH", "SELL_CLOSE_MARKET_BOTH",
  };
  return static_cast<std::uint8_t>(m) < 24 ? kNames[static_cast<std::uint8_t>(m)] : "?";
}

// ── private-data response type (informational; events carry it as uint8) ────
enum class PrivateOrderRespType : std::uint8_t {
  WS_REQUEST_PLACE_ORDER = 0,
  WS_REQUEST_CANCEL_ORDER = 1,
  WS_REQUEST_MODIFY_ORDER = 2,
  WS_REQUEST_QUERY_ORDER = 3,
  WS_ORDER_UPDATE = 4,
  WS_TRADE_LITE = 5,
  Count,
};

// ── order lifecycle status (events' `status` byte) ──────────────────────────
enum class OrderStatus : std::uint8_t {
  PLACE_CREATED = 0,    // 本地下单已生成, 尚未收到交易所确认
  CANCEL_CREATED = 1,   // 本地撤单已生成, 尚未收到交易所确认
  NEW = 2,              // 新订单已被引擎接受
  PARTIALLY_FILLED = 3, // 部分成交
  FILLED = 4,           // 完全成交
  CANCELED = 5,         // 用户取消
  REJECTED = 6,         // 撤消挂单再下单流程中, 新下单被拒
  EXPIRED = 7,          // 按 Time In Force 规则过期取消
  PARTIALLY_FILLED_TRADE_LITE = 8,
  UNKNOWN_ORDER_STATUS = 9,
  Count,
};

inline bool is_order_on_line(OrderStatus s) noexcept {
  return s == OrderStatus::NEW || s == OrderStatus::PARTIALLY_FILLED;
}

[[nodiscard]] constexpr const char *to_string(OrderStatus s) noexcept {
  switch (s) {
  case OrderStatus::PLACE_CREATED:               return "PLACE_CREATED";
  case OrderStatus::CANCEL_CREATED:              return "CANCEL_CREATED";
  case OrderStatus::NEW:                         return "NEW";
  case OrderStatus::PARTIALLY_FILLED:            return "PARTIALLY_FILLED";
  case OrderStatus::FILLED:                      return "FILLED";
  case OrderStatus::CANCELED:                    return "CANCELED";
  case OrderStatus::REJECTED:                    return "REJECTED";
  case OrderStatus::EXPIRED:                     return "EXPIRED";
  case OrderStatus::PARTIALLY_FILLED_TRADE_LITE: return "PARTIALLY_FILLED_TRADE_LITE";
  case OrderStatus::UNKNOWN_ORDER_STATUS:        return "UNKNOWN_ORDER_STATUS";
  case OrderStatus::Count:                       return "?";   // 终结项,非实际状态
  }
  return "?";
}

} // namespace gconf::enums
