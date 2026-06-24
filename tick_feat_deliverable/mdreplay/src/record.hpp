#pragma once
// record.hpp — 格式无关的中间语。输入 seam(csv/json)产出它,输出 seam(book/trade sink)消费它,
// 核心(merge/clock)只搬运它 —— 三段彻底解耦的关键。
//
// 价/量是 double 原值,编码成 uint32×10^scale 在输出侧(scale.hpp)做;ts 全程 epoch ns。

#include <cstdint>

namespace mdreplay {

enum class Kind : std::uint8_t { Book, Trade };

struct Record {
  std::int64_t  ts_ns{0};    // epoch 纳秒(= gconf exch_ns)
  std::uint16_t gid{0};      // gconf global_symbol_id
  Kind          kind{Kind::Book};

  // Book(BBO):kind==Book 时有效
  double bid_px{0.0}, bid_qty{0.0}, ask_px{0.0}, ask_qty{0.0};

  // Trade:kind==Trade 时有效
  std::uint8_t side{0};      // 0 = buy / 1 = sell
  double       px{0.0}, qty{0.0};
};

}  // namespace mdreplay
