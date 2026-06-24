#pragma once
// json.hpp — 输出去向之一:把 Record 序列化成 JSONL 文件(每行一对象)。
// 价/量为**字符串**(沿用精度,与 json 输入契约对称);symbol 由 gid 还原。

#include <fstream>
#include <memory>
#include <string>

#include <gconf/domain/symbols.h>

#include "core/error.hpp"
#include "core/fixed.hpp"
#include "core/record.hpp"
#include "output/sink.hpp"

namespace mdreplay {

class JsonSink : public Sink {
public:
  static Result<std::unique_ptr<JsonSink>> open(const std::string& path, Kind /*kind*/) {
    auto f = std::make_unique<std::ofstream>(path);
    if (!*f) return std::unexpected(Error::OutputOpen);
    return std::unique_ptr<JsonSink>(new JsonSink(std::move(f)));
  }

  Result<void> write(const Record& r) override {
    const auto sym = gconf::sym::global_symbol_id_name(r.gid);
    auto&      o   = *out_;
    o << "{\"ts\":" << r.ts_ns << ",\"symbol\":\"" << sym << "\",";
    if (r.kind == Kind::Book)
      o << "\"bid_px\":\"" << to_decimal(r.bid_px, r.price_scale) << "\",\"bid_qty\":\""
        << to_decimal(r.bid_qty, r.qty_scale) << "\",\"ask_px\":\""
        << to_decimal(r.ask_px, r.price_scale) << "\",\"ask_qty\":\""
        << to_decimal(r.ask_qty, r.qty_scale) << "\"}\n";
    else
      o << "\"side\":" << static_cast<int>(r.side) << ",\"px\":\""
        << to_decimal(r.px, r.price_scale) << "\",\"qty\":\"" << to_decimal(r.qty, r.qty_scale)
        << "\"}\n";
    return {};
  }

private:
  explicit JsonSink(std::unique_ptr<std::ofstream> f) : out_(std::move(f)) {}
  std::unique_ptr<std::ofstream> out_;
};

}  // namespace mdreplay
