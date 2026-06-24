#pragma once
// csv.hpp — RFC4180 CSV → 有序 Record 源(解析委托 csvparser:引号/转义/CRLF 全处理)。
//
// 列按名取(csvparser 表头驱动);数值仍走 fixed.hpp 定点编码(不碰 double,保 bit-exact)。
// 坏行 / 字段数不符 / 未知符号 → 计数跳过,不中断;表头缺必需列 / 文件打不开 → 上抛。
// 批量载入(回放数据全在手,k 路归并需同时 peek;惰性流式留待按需)。

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <csv.hpp>
#include <spdlog/spdlog.h>

#include "core/error.hpp"
#include "core/record.hpp"
#include "core/skip.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {
namespace detail {

// 严格整数解析:必须整串消费(避免 "123abc" 被静默接受);用于 ts / side。
[[nodiscard]] inline std::optional<std::int64_t> to_i64(std::string_view s) {
  std::int64_t v{};
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

}  // namespace detail

// book 各档 4 列的列名(在表头阶段一次性解析,免每行拼字符串)。[0..depth) 有效。
struct BookColumns {
  std::size_t                        depth = 0;
  std::array<std::string, kMaxDepth> bid_px, bid_qty, ask_px, ask_qty;
};

// 开 CSV reader:显式格式(不让 guess_csv 猜分隔符)——逗号、首行表头、保留列数不符的行
// (KEEP 把 ragged 行交回调用方按 row.size() 计数跳过,而非让库静默吞掉,保 skipped 可观测)。
[[nodiscard]] inline Result<std::unique_ptr<csv::CSVReader>> open_csv_reader(const std::string& path) {
  csv::CSVFormat fmt;
  fmt.delimiter(',');
  fmt.header_row(0);
  fmt.variable_columns(csv::VariableColumnPolicy::KEEP);
  try {
    return std::make_unique<csv::CSVReader>(path, fmt);
  } catch (const std::exception&) {
    return std::unexpected(Error::FileOpen);
  }
}

// 自动识别 book 档数(只 1 或 5)并解析各档列名。level0 不带后缀(= 旧 BBO);_1.._4 为高档。
// 无 _1.. 标记 → 1 档;_1.._4 标记齐且无 _5 → 5 档;残缺(部分档/某档四列不全)或 >5 → CsvSchema。
template <class HasFn>
[[nodiscard]] inline Result<BookColumns> resolve_book_columns(HasFn has) {
  if (!has("bid_px") || !has("bid_qty") || !has("ask_px") || !has("ask_qty"))
    return std::unexpected(Error::CsvSchema);
  const auto marker = [&](std::size_t k) { return has("bid_px_" + std::to_string(k)); };
  BookColumns cols;
  if (!marker(1) && !marker(2) && !marker(3) && !marker(4) && !marker(5)) cols.depth = 1;
  else if (marker(1) && marker(2) && marker(3) && marker(4) && !marker(5)) cols.depth = 5;
  else return std::unexpected(Error::CsvSchema);  // 残缺档 或 >5 档
  for (std::size_t k = 0; k < cols.depth; ++k) {
    const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
    if (!has("bid_px" + s) || !has("bid_qty" + s) || !has("ask_px" + s) || !has("ask_qty" + s))
      return std::unexpected(Error::CsvSchema);  // 某档四列不全 → 残缺
    cols.bid_px[k] = "bid_px" + s;  cols.bid_qty[k] = "bid_qty" + s;
    cols.ask_px[k] = "ask_px" + s;  cols.ask_qty[k] = "ask_qty" + s;
  }
  return cols;
}

// 表头驱动加载一个 CSV 文件 → Source。book 档数自动识别(1 或 5,残缺/>5 拒整文件,绝不截断);
// skips 按原因累加坏/未知行。文件/表头错误 → 上抛。
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_csv_source(const std::string& path,
                                                                     Kind kind, SkipStats& skips) {
  auto reader = open_csv_reader(path);
  if (!reader) return std::unexpected(reader.error());

  // 列校验(一次性,在迭代前):缺表头 / 缺必需列 → CsvSchema 上抛。
  if ((*reader)->get_col_names().empty()) return std::unexpected(Error::CsvSchema);
  const auto has = [&](csv::string_view c) { return (*reader)->index_of(c) >= 0; };
  if (!has("ts") || !has("symbol")) return std::unexpected(Error::CsvSchema);

  BookColumns cols;
  if (kind == Kind::Book) {
    auto c = resolve_book_columns(has);
    if (!c) return std::unexpected(c.error());
    cols = std::move(*c);
  } else if (!has("side") || !has("px") || !has("qty")) {
    return std::unexpected(Error::CsvSchema);
  }
  const std::size_t n_cols = (*reader)->get_col_names().size();

  std::vector<Record> rows;
  try {
    std::array<std::string_view, kMaxDepth> bpx, bqty, apx, aqty;  // 逐行复用的档位缓冲
    for (csv::CSVRow& row : **reader) {
      if (row.size() != n_cols) { skips.add(SkipReason::Malformed); continue; }  // 列数不符(先护栏)

      const auto ts = detail::to_i64(row["ts"].get_sv());
      if (!ts) { skips.add(SkipReason::BadTimestamp); continue; }

      const auto sym = row["symbol"].get_sv();
      std::expected<Record, SkipReason> rec = std::unexpected(SkipReason::BadField);  // 默认:side 坏
      if (kind == Kind::Book) {
        for (std::size_t k = 0; k < cols.depth; ++k) {
          bpx[k]  = row[cols.bid_px[k]].get_sv();  bqty[k] = row[cols.bid_qty[k]].get_sv();
          apx[k]  = row[cols.ask_px[k]].get_sv();  aqty[k] = row[cols.ask_qty[k]].get_sv();
        }
        rec = make_book_record(*ts, sym, std::span(bpx).first(cols.depth),
                               std::span(bqty).first(cols.depth), std::span(apx).first(cols.depth),
                               std::span(aqty).first(cols.depth));
      } else if (const auto side = detail::to_i64(row["side"].get_sv())) {
        rec = make_trade_record(*ts, sym, *side, row["px"].get_sv(), row["qty"].get_sv());
      }
      if (!rec) { account_build_failure(skips, rec.error(), sym); continue; }
      rows.push_back(*rec);
    }
  } catch (const std::exception&) {
    return std::unexpected(Error::CsvParse);  // 迭代期不可恢复解析错误(罕见)
  }

  spdlog::debug("csv '{}': {} 档, {} 行", path, cols.depth, rows.size());
  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
