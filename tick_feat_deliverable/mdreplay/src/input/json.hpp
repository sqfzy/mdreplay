#pragma once
// json.hpp — JSONL(每行一个 JSON 对象)→ 有序 Record 源。字段同 csv;价/量为**字符串**(沿用精度)。
// 例:{"ts":1782204110953000000,"symbol":"SOLUSDT","bid_px":"68.84","bid_qty":"1906.67",...}

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/error.hpp"
#include "core/record.hpp"
#include "core/skip.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {

// book 档数由每行对象的键**自动识别**,只接受 1 或满 5 档;残缺/>5 档 → BadField 跳过该行(不截断)。
// (json 无表头,逐行判定,行间可不同档,各按自身 1 或 5。)
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_json_source(const std::string& path,
                                                                      Kind kind, SkipStats& skips) {
  std::ifstream f(path);
  if (!f) return std::unexpected(Error::FileOpen);

  std::vector<Record> rows;
  std::string         line;
  // 逐行复用:持有字符串(string_view 指向它们)+ 视图缓冲。
  std::array<std::string, kMaxDepth>      sbpx, sbqty, sapx, saqty;
  std::array<std::string_view, kMaxDepth> bpx, bqty, apx, aqty;
  while (std::getline(f, line)) {
    chomp_cr(line);
    if (line.empty()) continue;

    const auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { skips.add(SkipReason::Malformed); continue; }

    // 缺字段 / 类型错 → json 异常,归 BadField(symbol 可能未取到,未知符号交给 make_*_record 分类)。
    std::string                       sym;
    std::expected<Record, SkipReason> rec = std::unexpected(SkipReason::BadField);
    try {
      const auto ts = j.at("ts").get<std::int64_t>();
      sym           = j.at("symbol").get<std::string>();
      if (kind == Kind::Book) {
        // 自动识别档数,只接受 1 或 5:无 _1.. 键 → 1;_1.._4 齐且无 _5 → 5;其它 → BadField 跳过。
        const auto m = [&](std::size_t k) { return j.contains("bid_px_" + std::to_string(k)); };
        std::size_t depth = 0;
        if (!m(1) && !m(2) && !m(3) && !m(4) && !m(5)) depth = 1;
        else if (m(1) && m(2) && m(3) && m(4) && !m(5)) depth = 5;
        else { skips.add(SkipReason::BadField); continue; }  // 残缺/>5 档
        for (std::size_t k = 0; k < depth; ++k) {
          const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
          sbpx[k]  = j.at("bid_px" + s).get<std::string>();  bpx[k]  = sbpx[k];
          sbqty[k] = j.at("bid_qty" + s).get<std::string>(); bqty[k] = sbqty[k];
          sapx[k]  = j.at("ask_px" + s).get<std::string>();  apx[k]  = sapx[k];
          saqty[k] = j.at("ask_qty" + s).get<std::string>(); aqty[k] = saqty[k];
        }
        rec = make_book_record(ts, sym, std::span(bpx).first(depth), std::span(bqty).first(depth),
                               std::span(apx).first(depth), std::span(aqty).first(depth));
      } else {
        rec = make_trade_record(ts, sym, j.at("side").get<std::int64_t>(),
                                j.at("px").get<std::string>(), j.at("qty").get<std::string>());
      }
    } catch (const nlohmann::json::exception&) {
      skips.add(SkipReason::BadField);
      continue;
    }
    if (!rec) { account_build_failure(skips, rec.error(), sym); continue; }
    rows.push_back(*rec);
  }

  spdlog::debug("json '{}': {} 行", path, rows.size());
  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
