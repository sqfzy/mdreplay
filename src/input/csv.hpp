#pragma once
// csv.hpp — 表头驱动 CSV → 有序 Record 源。批量载入(回放数据全在手,k 路归并需同时 peek)。
//
// 列按名取(不靠列位魔法);记录构建复用 record_build.hpp。坏行 / 未知符号 / 字段数不符 → 计数跳过。

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

#include "core/error.hpp"
#include "core/record.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {
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

// 表头驱动加载一个 CSV 文件 → Source。skipped 累加跳过的坏/未知行。文件/表头错误 → 上抛。
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
  std::vector<std::string_view> f_;
  while (std::getline(f, line)) {
    detail::chomp_cr(line);
    if (line.empty()) continue;
    detail::split_csv(line, f_);
    if (f_.size() != hcols.size()) { ++skipped; continue; }  // 字段数不符 → 跳

    const auto ts = detail::to_i64(f_[*c_ts]);
    if (!ts) { ++skipped; continue; }

    std::optional<Record> rec;
    if (kind == Kind::Book) {
      rec = make_book_record(*ts, f_[*c_sym], f_[*c_bpx], f_[*c_bqty], f_[*c_apx], f_[*c_aqty]);
    } else {
      const auto side = detail::to_i64(f_[*c_side]);
      if (side) rec = make_trade_record(*ts, f_[*c_sym], *side, f_[*c_px], f_[*c_qty]);
    }
    if (!rec) { ++skipped; continue; }
    rows.push_back(*rec);
  }

  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
