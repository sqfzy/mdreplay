#pragma once
// json.hpp — JSONL(每行一个 JSON 对象)→ 有序 Record 源。字段同 csv;价/量为**字符串**(沿用精度)。
// 例:{"ts":1782204110953000000,"symbol":"SOLUSDT","bid_px":"68.84","bid_qty":"1906.67",...}

#include <array>
#include <cstddef>
#include <charconv>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/error.hpp"
#include "core/record.hpp"
#include "core/skip.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {

// 流式 JSONL 源:持 ifstream,advance() 时才 getline+解析下一行(只缓冲 1 条 head_)。
// 峰值内存 O(1 条 + 一行)。book 档数逐行自动识别(只 1 或满 5,残缺/>5 → BadField 跳该行,不截断)。
class StreamingJsonSource : public Source {
public:
  StreamingJsonSource(std::ifstream f, Kind kind, SkipStats& skips, std::string path)
      : f_(std::move(f)), kind_(kind), skips_(&skips), path_(std::move(path)) {
    advance();  // 预读首条进 head_
  }

  [[nodiscard]] const Record* peek() override { return head_ ? &*head_ : nullptr; }

  void advance() override {
    head_.reset();
    while (std::getline(f_, line_)) {
      chomp_cr(line_);
      if (line_.empty()) continue;
      if (auto rec = build_from_line(line_)) { head_ = std::move(*rec); return; }
    }
  }

private:
  // 一行 JSONL → Record;跳过(已按原因归账)返回 nullopt。
  [[nodiscard]] std::optional<Record> build_from_line(const std::string& line) {
    const auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { skips_->add(SkipReason::Malformed); return std::nullopt; }

    // 缺字段 / 类型错 → json 异常,归 BadField(symbol 可能未取到,未知符号交给 make_*_record 分类)。
    std::string                       sym;
    std::expected<Record, SkipReason> rec = std::unexpected(SkipReason::BadField);
    try {
      const auto ts = j.at("ts").get<std::int64_t>();
      sym           = j.at("symbol").get<std::string>();
      if (kind_ == Kind::Book) {
        const auto present = [&](std::size_t k) { return j.contains("bid_px_" + std::to_string(k)); };
        const auto d       = book_depth_of(present);
        if (!d) {  // 残缺/>5 档:自描述告知一次(json 逐行,避免每行刷屏),仍按 BadField 跳该行
          if (!depth_warned_) {
            const std::size_t got = highest_book_level(present) + 1;
            spdlog::warn("json '{}': book 档数 {} 不受支持(只支持 {{1,5,10,15,20,25}} 档,不截断、不跳档);"
                         "该类行将按坏行跳过", path_, got);
            depth_warned_ = true;
          }
          skips_->add(SkipReason::BadField);
          return std::nullopt;
        }
        // update_id 容忍数字或字符串(book json 的 px/qty 本就是字符串,上游极可能也把它写成串)。
        const auto&   uidv = j.at("update_id");  // 缺失 → 异常 → BadField(下方 catch)
        std::uint64_t upd  = 0;
        if (uidv.is_string()) {
          const std::string& us = uidv.get_ref<const std::string&>();
          const auto [p, ec] = std::from_chars(us.data(), us.data() + us.size(), upd);
          if (ec != std::errc{} || p != us.data() + us.size()) {
            skips_->add(SkipReason::BadField);  // 字符串非纯数字
            return std::nullopt;
          }
        } else {
          upd = uidv.get<std::uint64_t>();  // 数字;非数字类型 → json 异常 → BadField
        }
        const std::size_t depth = *d;
        for (std::size_t k = 0; k < depth; ++k) {
          const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
          sbpx_[k]  = j.at("bid_px" + s).get<std::string>();  bpx_[k]  = sbpx_[k];
          sbqty_[k] = j.at("bid_qty" + s).get<std::string>(); bqty_[k] = sbqty_[k];
          sapx_[k]  = j.at("ask_px" + s).get<std::string>();  apx_[k]  = sapx_[k];
          saqty_[k] = j.at("ask_qty" + s).get<std::string>(); aqty_[k] = saqty_[k];
        }
        rec = make_book_record(ts, upd, sym, std::span(bpx_).first(depth), std::span(bqty_).first(depth),
                               std::span(apx_).first(depth), std::span(aqty_).first(depth));
      } else {
        rec = make_trade_record(ts, sym, j.at("side").get<std::int64_t>(),
                                j.at("px").get<std::string>(), j.at("qty").get<std::string>());
      }
    } catch (const nlohmann::json::exception&) {
      skips_->add(SkipReason::BadField);
      return std::nullopt;
    }
    if (!rec) { account_build_failure(*skips_, rec.error(), sym); return std::nullopt; }
    return *rec;
  }

  std::ifstream         f_;
  Kind                  kind_;
  SkipStats*            skips_;
  std::string           path_;               // 仅供报错定位
  bool                  depth_warned_{false};  // 档数不支持只自描述告知一次,免逐行刷屏
  std::string           line_;  // 逐 advance 复用
  std::optional<Record> head_;
  std::array<std::string, kMaxDepth>      sbpx_, sbqty_, sapx_, saqty_;  // 持有串(视图指向它们)
  std::array<std::string_view, kMaxDepth> bpx_, bqty_, apx_, aqty_;
};

// 开一个 JSONL 文件 → 流式 Source。文件打不开 → 上抛;坏/未知行在消费(advance)时归账到 skips。
// (json 无表头,逐行判定,行间可不同档,各按自身 1 或 5。)
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_json_source(const std::string& path,
                                                                      Kind kind, SkipStats& skips) {
  std::ifstream f(path);
  if (!f) return std::unexpected(Error::FileOpen);
  spdlog::debug("json '{}': 流式载入", path);
  return std::unique_ptr<Source>{std::make_unique<StreamingJsonSource>(std::move(f), kind, skips, path)};
}

}  // namespace mdreplay
