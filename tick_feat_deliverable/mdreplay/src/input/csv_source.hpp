#pragma once
// csv_source.hpp — 表头驱动 CSV → 有序 Record 源。批量载入(回放数据全在手,k 路归并需同时 peek)。
//
// 列按名取(不靠列位魔法);symbol→gid 用 gconf;价/量经 fixed.hpp 编码成定点(精度沿用数据)。
// 坏行 / 未知符号 / 字段数不符 → 计数跳过,不中断。book 与 trade 走同一加载器,kind 决定取哪些列。

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "error.hpp"
#include "fixed.hpp"
#include "gid.hpp"
#include "input/source.hpp"
#include "record.hpp"

namespace mdreplay {

// 已载入内存的有序 Record 源。
class LoadedSource : public Source {
public:
  explicit LoadedSource(std::vector<Record> rows) : rows_(std::move(rows)) {}
  [[nodiscard]] const Record* peek() override { return i_ < rows_.size() ? &rows_[i_] : nullptr; }
  void                        advance() override { ++i_; }

private:
  std::vector<Record> rows_;
  std::size_t         i_ = 0;
};

namespace detail {

// 去掉行尾 '\r'(健壮吃 CRLF 输入,如 Python csv.writer 的 \r\n)。
inline void chomp_cr(std::string& s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
}

inline void split_csv(std::string_view line, std::vector<std::string_view>& out) {
  out.clear();
  std::size_t start = 0;
  for (;;) {
    const auto comma = line.find(',', start);
    out.push_back(line.substr(start, comma == std::string_view::npos ? comma : comma - start));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
}

[[nodiscard]] inline std::optional<std::int64_t> to_i64(std::string_view s) {
  std::int64_t v{};
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

}  // namespace detail

// 表头驱动加载一个文件 → Source。skipped 累加跳过的坏/未知行。文件/表头错误 → 上抛。
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_csv_source(const std::string& path,
                                                                     Kind kind, std::size_t& skipped) {
  std::ifstream f(path);
  if (!f) return std::unexpected(Error::FileOpen);

  std::string header;
  if (!std::getline(f, header)) return std::unexpected(Error::CsvSchema);
  detail::chomp_cr(header);

  std::vector<std::string_view> hcols;
  detail::split_csv(header, hcols);
  std::unordered_map<std::string_view, std::size_t> idx;
  for (std::size_t i = 0; i < hcols.size(); ++i) idx.emplace(hcols[i], i);

  const auto col = [&](std::string_view name) -> std::optional<std::size_t> {
    const auto it = idx.find(name);
    return it == idx.end() ? std::nullopt : std::optional<std::size_t>{it->second};
  };

  const auto c_ts = col("ts");
  const auto c_sym = col("symbol");
  if (!c_ts || !c_sym) return std::unexpected(Error::CsvSchema);
  std::optional<std::size_t> c_bpx, c_bqty, c_apx, c_aqty, c_side, c_px, c_qty;
  if (kind == Kind::Book) {
    c_bpx = col("bid_px"); c_bqty = col("bid_qty"); c_apx = col("ask_px"); c_aqty = col("ask_qty");
    if (!c_bpx || !c_bqty || !c_apx || !c_aqty) return std::unexpected(Error::CsvSchema);
  } else {
    c_side = col("side"); c_px = col("px"); c_qty = col("qty");
    if (!c_side || !c_px || !c_qty) return std::unexpected(Error::CsvSchema);
  }

  std::vector<Record>           rows;
  std::string                   line;
  std::vector<std::string_view> fields;
  while (std::getline(f, line)) {
    detail::chomp_cr(line);
    if (line.empty()) continue;
    detail::split_csv(line, fields);
    if (fields.size() != hcols.size()) { ++skipped; continue; }  // 字段数不符 → 跳

    const auto ts  = detail::to_i64(fields[*c_ts]);
    const auto gid = gid_of(fields[*c_sym]);
    if (!ts || !gid) { ++skipped; continue; }

    Record r;
    r.kind  = kind;
    r.ts_ns = *ts;
    r.gid   = *gid;

    if (kind == Kind::Book) {
      std::uint32_t           pv[2], qv[2];
      const std::string_view  ps[2] = {fields[*c_bpx], fields[*c_apx]};
      const std::string_view  qs[2] = {fields[*c_bqty], fields[*c_aqty]};
      const auto psc = encode_group(ps, pv);
      const auto qsc = encode_group(qs, qv);
      if (!psc || !qsc) { ++skipped; continue; }
      r.price_scale = *psc; r.qty_scale = *qsc;
      r.bid_px = pv[0]; r.ask_px = pv[1]; r.bid_qty = qv[0]; r.ask_qty = qv[1];
    } else {
      const auto side = detail::to_i64(fields[*c_side]);
      std::uint32_t          pv[1], qv[1];
      const std::string_view ps[1] = {fields[*c_px]}, qs[1] = {fields[*c_qty]};
      const auto psc = encode_group(ps, pv);
      const auto qsc = encode_group(qs, qv);
      if (!side || (*side != 0 && *side != 1) || !psc || !qsc) { ++skipped; continue; }
      r.side = static_cast<std::uint8_t>(*side);
      r.price_scale = *psc; r.qty_scale = *qsc; r.px = pv[0]; r.qty = qv[0];
    }
    rows.push_back(r);
  }

  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
