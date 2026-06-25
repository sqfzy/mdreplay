#pragma once
// record.hpp — 格式无关的中间语。输入 seam(csv/json)产出它,输出 seam(book/trade sink)消费它,
// 核心(merge/clock)只搬运它 —— 三段彻底解耦的关键。
//
// 价/量以「定点 mantissa(uint32 ×10^scale)」承载,精度沿用数据本身(在 csv_source 由字符串推得,
// 不规定、不走 double)。book 的所有档价共用 price_scale、所有量共用 qty_scale(对齐 BoardSlot 单 scale)。
//
// 多档:book 携带 depth(1..kMaxDepth)档,bid/ask_px/qty 为按档升序(0=最优档)的定点数组。
// depth=1 即 BBO,与旧契约一致(level0 列名不带后缀,见 csv/json 解析)。

#include <array>
#include <cstdint>

namespace mdreplay {

enum class Kind : std::uint8_t { Book, Trade };

// book 最大档数。shm DepthBoard 段按此定长(POD 段须编译期定长);输入档数 >kMaxDepth 截断。
inline constexpr std::size_t kMaxDepth = 5;

struct Record {
  std::int64_t  ts_ns{0};            // epoch 纳秒(= gconf exch_ns)
  std::uint64_t update_id{0};        // 交易所盘口更新序号(book 段 BookTickBoard 需真值;trade 不用)
  std::uint16_t gid{0};              // gconf 符号 id(book 用作 LID 索引 slot、trade 用作 GID)
  Kind          kind{Kind::Book};
  std::uint8_t  price_scale{0};      // 价定点指数(数据天然精度,全档共用)
  std::uint8_t  qty_scale{0};        // 量定点指数(全档共用)
  std::uint8_t  depth{1};            // book 有效档数(1..kMaxDepth);trade 不用

  // Book:kind==Book 时有效。[0..depth) 有效,0=最优档。mantissa = 真值 ×10^*scale
  std::array<std::uint32_t, kMaxDepth> bid_px{}, bid_qty{}, ask_px{}, ask_qty{};

  // Trade:kind==Trade 时有效
  std::uint8_t  side{0};             // 0 = buy / 1 = sell
  std::uint32_t px{0}, qty{0};
};

}  // namespace mdreplay
