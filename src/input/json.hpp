#pragma once
// json.hpp — JSONL(每行一个 JSON 对象)→ 有序 Record 源。字段同 csv;价/量为**字符串**(沿用精度)。
// 例:{"ts":1782204110953000000,"symbol":"SOLUSDT","bid_px":"68.84","bid_qty":"1906.67",...}

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/error.hpp"
#include "core/record.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {

[[nodiscard]] inline Result<std::unique_ptr<Source>> load_json_source(const std::string& path,
                                                                      Kind kind, std::size_t& skipped) {
  std::ifstream f(path);
  if (!f) return std::unexpected(Error::FileOpen);

  std::vector<Record> rows;
  std::string         line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    const auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { ++skipped; continue; }

    std::optional<Record> rec;
    try {
      const auto ts  = j.at("ts").get<std::int64_t>();
      const auto sym = j.at("symbol").get<std::string>();
      if (kind == Kind::Book) {
        rec = make_book_record(ts, sym, j.at("bid_px").get<std::string>(),
                               j.at("bid_qty").get<std::string>(), j.at("ask_px").get<std::string>(),
                               j.at("ask_qty").get<std::string>());
      } else {
        rec = make_trade_record(ts, sym, j.at("side").get<std::int64_t>(),
                                j.at("px").get<std::string>(), j.at("qty").get<std::string>());
      }
    } catch (const nlohmann::json::exception&) {
      ++skipped;
      continue;
    }
    if (!rec) { ++skipped; continue; }
    rows.push_back(*rec);
  }

  return std::unique_ptr<Source>{std::make_unique<LoadedSource>(std::move(rows))};
}

}  // namespace mdreplay
