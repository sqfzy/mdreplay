#pragma once
// record_build.hpp — 字段(已拆好的字符串)→ Record。csv 与 json 两个解析器共用的输入契约逻辑。
//
// symbol→gid 用 gconf;价/量经 fixed.hpp 编码成定点(精度沿用数据);非法/未知符号 → nullopt
// (调用方计数跳过)。book 的 bid/ask 用公共 price_scale、两量用公共 qty_scale。

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/fixed.hpp"
#include "core/gid.hpp"
#include "core/record.hpp"

namespace mdreplay {

[[nodiscard]] inline std::optional<Record> make_book_record(std::int64_t ts, std::string_view sym,
                                                            std::string_view bid_px,
                                                            std::string_view bid_qty,
                                                            std::string_view ask_px,
                                                            std::string_view ask_qty) {
  const auto gid = gid_of(sym);
  if (!gid) return std::nullopt;
  std::uint32_t          pv[2]{}, qv[2]{};
  const std::string_view ps[2] = {bid_px, ask_px};
  const std::string_view qs[2] = {bid_qty, ask_qty};
  const auto psc = encode_group(ps, pv);
  const auto qsc = encode_group(qs, qv);
  if (!psc || !qsc) return std::nullopt;
  Record r;
  r.kind = Kind::Book;
  r.ts_ns = ts;
  r.gid = *gid;
  r.price_scale = *psc;
  r.qty_scale = *qsc;
  r.bid_px = pv[0];
  r.ask_px = pv[1];
  r.bid_qty = qv[0];
  r.ask_qty = qv[1];
  return r;
}

[[nodiscard]] inline std::optional<Record> make_trade_record(std::int64_t ts, std::string_view sym,
                                                             std::int64_t side, std::string_view px,
                                                             std::string_view qty) {
  if (side != 0 && side != 1) return std::nullopt;
  const auto gid = gid_of(sym);
  if (!gid) return std::nullopt;
  std::uint32_t          pv[1]{}, qv[1]{};
  const std::string_view ps[1] = {px}, qs[1] = {qty};
  const auto psc = encode_group(ps, pv);
  const auto qsc = encode_group(qs, qv);
  if (!psc || !qsc) return std::nullopt;
  Record r;
  r.kind = Kind::Trade;
  r.ts_ns = ts;
  r.gid = *gid;
  r.side = static_cast<std::uint8_t>(side);
  r.price_scale = *psc;
  r.qty_scale = *qsc;
  r.px = pv[0];
  r.qty = qv[0];
  return r;
}

}  // namespace mdreplay
