#pragma once
// record_build.hpp — 字段(已拆好的字符串)→ Record。csv 与 json 两个解析器共用的输入契约逻辑。
//
// symbol→gid 用 gconf;价/量经 fixed.hpp 编码成定点(精度沿用数据)。失败返回**分类的** SkipReason
// (未知符号 / 数值非法 / scale 溢出),让调用方按原因归账而非笼统跳过。
// book 的 bid/ask 用公共 price_scale、两量用公共 qty_scale。

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "core/error.hpp"
#include "core/fixed.hpp"
#include "core/gid.hpp"
#include "core/record.hpp"
#include "core/skip.hpp"

namespace mdreplay {

// fixed.hpp 的编码错误 → 跳过原因:scale 越界单列,其余归数值非法。
[[nodiscard]] inline SkipReason skip_of(Error e) noexcept {
  return e == Error::ScaleOverflow ? SkipReason::ScaleOverflow : SkipReason::BadNumber;
}

// book 档数判定(唯一真相源,csv 表头与 json 每行键共用):只接受 1 或 5,残缺/>5 → nullopt。
// marker(k) 报第 k 档是否存在(k∈1..5)。无 _1.. → 1 档;_1.._4 齐且无 _5 → 5 档;其余拒。
template <class MarkerFn>
[[nodiscard]] inline std::optional<std::size_t> book_depth_of(MarkerFn marker) {
  if (!marker(1) && !marker(2) && !marker(3) && !marker(4) && !marker(5)) return std::size_t{1};
  if (marker(1) && marker(2) && marker(3) && marker(4) && !marker(5)) return std::size_t{5};
  return std::nullopt;  // 残缺档 或 >5 档
}

// 多档 book:每个 span 含 depth 档(0=最优)。全档价共用 price_scale、全档量共用 qty_scale
// (把 bid/ask 全档拼成一组编码,对齐 slot 单 scale);depth=1 即退化为 BBO,与旧契约逐字一致。
[[nodiscard]] inline std::expected<Record, SkipReason> make_book_record(
    std::int64_t ts, std::string_view sym, std::span<const std::string_view> bid_px,
    std::span<const std::string_view> bid_qty, std::span<const std::string_view> ask_px,
    std::span<const std::string_view> ask_qty) {
  const auto gid = gid_of(sym);
  if (!gid) return std::unexpected(SkipReason::UnknownSymbol);
  const std::size_t depth = bid_px.size();  // 调用方保证 4 个 span 等长且 ∈[1,kMaxDepth]

  // 价组 = [bid0..bid_{d-1}, ask0..ask_{d-1}];量组同序。
  std::array<std::string_view, 2 * kMaxDepth> px_in, qty_in;
  std::array<std::uint32_t, 2 * kMaxDepth>    px_out{}, qty_out{};
  for (std::size_t k = 0; k < depth; ++k) {
    px_in[k]  = bid_px[k];  px_in[depth + k]  = ask_px[k];
    qty_in[k] = bid_qty[k]; qty_in[depth + k] = ask_qty[k];
  }
  const auto psc = encode_group(std::span(px_in).first(2 * depth), std::span(px_out).first(2 * depth));
  if (!psc) return std::unexpected(skip_of(psc.error()));
  const auto qsc = encode_group(std::span(qty_in).first(2 * depth), std::span(qty_out).first(2 * depth));
  if (!qsc) return std::unexpected(skip_of(qsc.error()));

  Record r;
  r.kind        = Kind::Book;
  r.ts_ns       = ts;
  r.gid         = *gid;
  r.depth       = static_cast<std::uint8_t>(depth);
  r.price_scale = *psc;
  r.qty_scale   = *qsc;
  for (std::size_t k = 0; k < depth; ++k) {
    r.bid_px[k]  = px_out[k];      r.ask_px[k]  = px_out[depth + k];
    r.bid_qty[k] = qty_out[k];     r.ask_qty[k] = qty_out[depth + k];
  }
  return r;
}

[[nodiscard]] inline std::expected<Record, SkipReason> make_trade_record(
    std::int64_t ts, std::string_view sym, std::int64_t side, std::string_view px,
    std::string_view qty) {
  if (side != 0 && side != 1) return std::unexpected(SkipReason::BadField);
  const auto gid = gid_of(sym);
  if (!gid) return std::unexpected(SkipReason::UnknownSymbol);
  std::uint32_t          pv[1]{}, qv[1]{};
  const std::string_view ps[1] = {px}, qs[1] = {qty};
  const auto psc = encode_group(ps, pv);
  if (!psc) return std::unexpected(skip_of(psc.error()));
  const auto qsc = encode_group(qs, qv);
  if (!qsc) return std::unexpected(skip_of(qsc.error()));
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
