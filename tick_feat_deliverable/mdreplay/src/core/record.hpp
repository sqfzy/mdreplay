#pragma once
// record.hpp — 格式无关的中间语。输入 seam(csv/json)产出它,输出 seam(book/trade sink)消费它,
// 核心(merge/clock)只搬运它 —— 三段彻底解耦的关键。
//
// 价/量以「定点 mantissa(uint32 ×10^scale)」承载,精度沿用数据本身(在 csv_source 由字符串推得,
// 不规定、不走 double)。book 的 bid/ask 共用 price_scale、两个量共用 qty_scale(对齐 BoardSlot)。

#include <cstdint>

namespace mdreplay {

enum class Kind : std::uint8_t { Book, Trade };

struct Record {
  std::int64_t  ts_ns{0};            // epoch 纳秒(= gconf exch_ns)
  std::uint16_t gid{0};              // gconf global_symbol_id
  Kind          kind{Kind::Book};
  std::uint8_t  price_scale{0};      // 价定点指数(数据天然精度)
  std::uint8_t  qty_scale{0};        // 量定点指数

  // Book(BBO):kind==Book 时有效。mantissa = 真值 ×10^*scale
  std::uint32_t bid_px{0}, bid_qty{0}, ask_px{0}, ask_qty{0};

  // Trade:kind==Trade 时有效
  std::uint8_t  side{0};             // 0 = buy / 1 = sell
  std::uint32_t px{0}, qty{0};
};

}  // namespace mdreplay
