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
  std::array<std::string, kMaxDepth> bid_px, bid_qty, ask_px, ask_qty;
};

// 表头驱动加载一个 CSV 文件 → Source。book 档数由表头**自动识别**,只接受 1(纯 BBO)或满 5 档;
// 残缺(部分档)或 >5 档 → CsvSchema 拒整文件(绝不截断)。skips 按原因累加坏/未知行。文件/表头错误 → 上抛。
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_csv_source(const std::string& path,
                                                                     Kind kind, SkipStats& skips) {
  // 显式格式(不让 guess_csv 猜分隔符):逗号、首行表头、保留列数不符的行——
  // KEEP 把 ragged 行交回我们按 row.size() 计数跳过,而非让库静默吞掉(保 skipped 可观测)。
  csv::CSVFormat fmt;
  fmt.delimiter(',');
  fmt.header_row(0);
  fmt.variable_columns(csv::VariableColumnPolicy::KEEP);

  std::unique_ptr<csv::CSVReader> reader;
  try {
    reader = std::make_unique<csv::CSVReader>(path, fmt);
  } catch (const std::exception&) {
    return std::unexpected(Error::FileOpen);
  }

  // 列校验(一次性,在迭代前):缺表头 / 缺必需列 → CsvSchema 上抛。
  if (reader->get_col_names().empty()) return std::unexpected(Error::CsvSchema);
  const auto has = [&](csv::string_view c) { return reader->index_of(c) >= 0; };
  if (!has("ts") || !has("symbol")) return std::unexpected(Error::CsvSchema);

  // book:level0 列名不带后缀(= 旧 BBO,零迁移);level k≥1 为 bid_px_k 等。
  // 自动识别档数,只接受 1 或 5:无 _1.. 标记 → 1 档;_1.._4 标记齐且无 _5 → 5 档;
  // 其它(残缺/>5)→ CsvSchema 拒绝。预存各档列名免每行拼串。
  BookColumns cols;
  std::size_t depth = 0;
  if (kind == Kind::Book) {
    if (!has("bid_px") || !has("bid_qty") || !has("ask_px") || !has("ask_qty"))
      return std::unexpected(Error::CsvSchema);
    const auto m = [&](std::size_t k) { return has("bid_px_" + std::to_string(k)); };  // 档存在标记
    if (!m(1) && !m(2) && !m(3) && !m(4) && !m(5)) depth = 1;
    else if (m(1) && m(2) && m(3) && m(4) && !m(5)) depth = 5;
    else return std::unexpected(Error::CsvSchema);  // 残缺档 或 >5 档
    for (std::size_t k = 0; k < depth; ++k) {
      const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
      if (!has("bid_px" + s) || !has("bid_qty" + s) || !has("ask_px" + s) || !has("ask_qty" + s))
        return std::unexpected(Error::CsvSchema);  // 5 档时某档四列不全 → 残缺
      cols.bid_px[k] = "bid_px" + s;  cols.bid_qty[k] = "bid_qty" + s;
      cols.ask_px[k] = "ask_px" + s;  cols.ask_qty[k] = "ask_qty" + s;
    }
  } else {
    if (!has("side") || !has("px") || !has("qty")) return std::unexpected(Error::CsvSchema);
  }
  const std::size_t n_cols = reader->get_col_names().size();

  std::vector<Record> rows;
  try {
    std::array<std::string_view, kMaxDepth> bpx, bqty, apx, aqty;  // 逐行复用的档位缓冲
    for (csv::CSVRow& row : *reader) {
      if (row.size() != n_cols) { skips.add(SkipReason::Malformed); continue; }  // 列数不符(先护栏)

      const auto ts = detail::to_i64(row["ts"].get_sv());
      if (!ts) { skips.add(SkipReason::BadTimestamp); continue; }

      const auto sym = row["symbol"].get_sv();
      std::expected<Record, SkipReason> rec = std::unexpected(SkipReason::BadField);  // 默认:side 坏
      if (kind == Kind::Book) {
        for (std::size_t k = 0; k < depth; ++k) {
          bpx[k]  = row[cols.bid_px[k]].get_sv();  bqty[k] = row[cols.bid_qty[k]].get_sv();
          apx[k]  = row[cols.ask_px[k]].get_sv();  aqty[k] = row[cols.ask_qty[k]].get_sv();
        }
        rec = make_book_record(*ts, sym, std::span(bpx).first(depth), std::span(bqty).first(depth),
                               std::span(apx).first(depth), std::span(aqty).first(depth));
      } else if (const auto side = detail::to_i64(row["side"].get_sv())) {
        rec = make_trade_record(*ts, sym, *side, row["px"].get_sv(), row["qty"].get_sv());
      }
      if (!rec) { account_build_failure(skips, rec.error(), sym); continue; }
      rows.push_back(*rec);
    }
  } catch (const std::exception&) {
    return std::unexpected(Error::CsvParse);  // 迭代期不可恢复解析错误(罕见)
  }

  spdlog::debug("csv '{}': {} 档, {} 行", path, depth, rows.size());
  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
