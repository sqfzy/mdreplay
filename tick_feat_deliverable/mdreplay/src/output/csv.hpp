#pragma once
// csv.hpp — 输出去向之一:把 Record 序列化成 CSV 文件(格式转换 / 落盘 / 调试)。
// 按 kind 写对应表头;symbol 由 gid 还原;价/量由定点 to_decimal 还原成十进制(沿用精度)。

#include <fstream>
#include <memory>
#include <string>

#include <gconf/domain/symbols.h>

#include "core/error.hpp"
#include "core/fixed.hpp"
#include "core/record.hpp"
#include "output/sink.hpp"

namespace mdreplay {

class CsvSink : public Sink {
public:
  // depth(1 或 5)决定 book 表头档数;level0 列名不带后缀(= BBO),k≥1 为 bid_px_k 等。
  static Result<std::unique_ptr<CsvSink>> open(const std::string& path, Kind kind, std::size_t depth) {
    auto f = std::make_unique<std::ofstream>(path);
    if (!*f) return std::unexpected(Error::OutputOpen);
    if (kind == Kind::Book) {
      std::string hdr = "ts,symbol";
      for (std::size_t k = 0; k < depth; ++k) {
        const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
        hdr += ",bid_px" + s + ",bid_qty" + s + ",ask_px" + s + ",ask_qty" + s;
      }
      *f << hdr << '\n';
    } else {
      *f << "ts,symbol,side,px,qty\n";
    }
    return std::unique_ptr<CsvSink>(new CsvSink(std::move(f)));
  }

  Result<void> write(const Record& r) override {
    const auto sym = gconf::sym::global_symbol_id_name(r.gid);
    if (r.kind == Kind::Book) {
      *out_ << r.ts_ns << ',' << sym;
      for (std::size_t k = 0; k < r.depth; ++k)  // 逐档:bid_px,bid_qty,ask_px,ask_qty
        *out_ << ',' << to_decimal(r.bid_px[k], r.price_scale) << ','
              << to_decimal(r.bid_qty[k], r.qty_scale) << ','
              << to_decimal(r.ask_px[k], r.price_scale) << ','
              << to_decimal(r.ask_qty[k], r.qty_scale);
      *out_ << '\n';
    } else {
      *out_ << r.ts_ns << ',' << sym << ',' << static_cast<int>(r.side) << ','
            << to_decimal(r.px, r.price_scale) << ',' << to_decimal(r.qty, r.qty_scale) << '\n';
    }
    return {};
  }

private:
  explicit CsvSink(std::unique_ptr<std::ofstream> f) : out_(std::move(f)) {}
  std::unique_ptr<std::ofstream> out_;
};

}  // namespace mdreplay
