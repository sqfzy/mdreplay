#pragma once
// csv.hpp — 表头驱动 CSV → **流式**有序 Record 源。手写逐行读(getline + 引号感知切分),
// 峰值已提交内存 O(1 条 + 1 行),与文件大小/总行数无关——多天连续回放不 OOM 的关键。
//
// 切分支持 RFC4180-lite:`"..."` 包裹(内含逗号)、`""` 转义为 `"`;不支持字段内换行(逐行读)。
// 无引号的行走零拷贝快路(string_view 直指行缓冲);含引号的行去引号到 owned 缓冲。
// 数值仍走 fixed.hpp 定点(不碰 double),保 bit-exact。坏/未知行在消费(advance)时按原因归账。

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/error.hpp"
#include "core/record.hpp"
#include "core/skip.hpp"
#include "input/record_build.hpp"
#include "input/source.hpp"

namespace mdreplay {
namespace detail {

// 去前后空白(列名容错:手写 "ts, symbol" 也能匹配)。
[[nodiscard]] inline std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// 严格整数解析:必须整串消费(避免 "123abc" 被静默接受);用于 ts / side。
[[nodiscard]] inline std::optional<std::int64_t> to_i64(std::string_view s) {
  std::int64_t v{};
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// 同上,无符号(用于 update_id —— 语义 u64;from_chars 对 u64 天然拒负数)。
[[nodiscard]] inline std::optional<std::uint64_t> to_u64(std::string_view s) {
  std::uint64_t v{};
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// 读一个引号字段(line[i]=='"'):去引号、`""`→`"`,推进 i 到字段末逗号/行尾,返回去引号内容。
[[nodiscard]] inline std::string read_quoted_field(std::string_view line, std::size_t& i) {
  const std::size_t n = line.size();
  std::string       field;
  ++i;  // 跳开头引号
  while (i < n) {
    if (line[i] == '"') {
      if (i + 1 < n && line[i + 1] == '"') { field += '"'; i += 2; }  // "" → "
      else { ++i; break; }                                            // 闭合引号
    } else field += line[i++];
  }
  while (i < n && line[i] != ',') ++i;  // 跳到字段末逗号
  return field;
}

// 行 → 字段视图。无引号 → 零拷贝(视图指向 line);含引号 → 去引号到 owned,视图指向 owned。
// fields/owned 均复用(清空重填),峰值 O(1 行)。视图生命周期 = line(快路)/ owned(慢路)。
inline void split_csv(std::string_view line, std::vector<std::string_view>& fields,
                      std::vector<std::string>& owned) {
  fields.clear();
  if (line.find('"') == std::string_view::npos) {  // 快路:无引号,零拷贝逗号切分
    std::size_t start = 0;
    for (;;) {
      const auto comma = line.find(',', start);
      fields.push_back(line.substr(start, comma == std::string_view::npos ? comma : comma - start));
      if (comma == std::string_view::npos) break;
      start = comma + 1;
    }
    return;
  }
  owned.clear();  // 慢路:含引号,逐字段去引号
  const std::size_t n = line.size();
  std::size_t       i = 0;
  for (;;) {
    std::string field;
    if (i < n && line[i] == '"') {
      field = read_quoted_field(line, i);
    } else {  // 普通字段:到下一个逗号/行尾
      const auto comma = line.find(',', i);
      const auto end   = (comma == std::string_view::npos) ? n : comma;
      field.assign(line.substr(i, end - i));
      i = end;
    }
    owned.push_back(std::move(field));
    if (i < n && line[i] == ',') { ++i; continue; }  // 还有下一字段
    break;
  }
  for (const auto& s : owned) fields.push_back(s);  // owned 停止增长后再取视图,无失效
}

}  // namespace detail

// 一行各需求字段在切分结果里的列下标(表头阶段一次解析)。book [0..depth) 有效。
struct CsvCols {
  std::size_t                        ts = 0, symbol = 0;
  std::size_t                        side = 0, px = 0, qty = 0;  // trade
  std::size_t                        depth = 0, update_id = 0;   // book(档数 + 更新序号列)
  std::array<std::size_t, kMaxDepth> bid_px{}, bid_qty{}, ask_px{}, ask_qty{};
};

// 从表头列名(已 trim)解析各需求字段下标。book 档数自动识别(只 1 或 5,残缺/>5 → CsvSchema)。
[[nodiscard]] inline Result<CsvCols> resolve_csv_cols(const std::vector<std::string>& names, Kind kind) {
  const auto idx = [&](std::string_view name) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < names.size(); ++i)
      if (names[i] == name) return i;
    return std::nullopt;
  };
  CsvCols c;
  const auto ts = idx("ts"), sym = idx("symbol");
  if (!ts || !sym) return std::unexpected(Error::CsvSchema);
  c.ts = *ts;
  c.symbol = *sym;
  if (kind == Kind::Book) {
    const auto uid = idx("update_id");  // book 段 BookTickBoard 需交易所更新序号真值
    if (!uid) return std::unexpected(Error::CsvSchema);
    c.update_id = *uid;
    const auto present = [&](std::size_t k) { return idx("bid_px_" + std::to_string(k)).has_value(); };
    const auto depth   = book_depth_of(present);
    if (!depth) return std::unexpected(Error::BookDepthUnsupported);  // 残缺/>5 档,自描述报错在 load_csv_source
    c.depth = *depth;
    for (std::size_t k = 0; k < c.depth; ++k) {
      const std::string s = (k == 0) ? "" : "_" + std::to_string(k);
      const auto bp = idx("bid_px" + s), bq = idx("bid_qty" + s), ap = idx("ask_px" + s), aq = idx("ask_qty" + s);
      if (!bp || !bq || !ap || !aq) return std::unexpected(Error::CsvSchema);  // 某档四列不全
      c.bid_px[k] = *bp;  c.bid_qty[k] = *bq;  c.ask_px[k] = *ap;  c.ask_qty[k] = *aq;
    }
  } else {
    const auto sd = idx("side"), px = idx("px"), q = idx("qty");
    if (!sd || !px || !q) return std::unexpected(Error::CsvSchema);
    c.side = *sd;  c.px = *px;  c.qty = *q;
  }
  return c;
}

// 流式 CSV 源:持 ifstream(表头已消费)+ 列下标,advance() 时才 getline+切分下一行(只缓冲 1 条)。
class StreamingCsvSource : public Source {
public:
  StreamingCsvSource(std::ifstream f, Kind kind, CsvCols cols, std::size_t n_cols, SkipStats& skips)
      : f_(std::move(f)), kind_(kind), cols_(cols), n_cols_(n_cols), skips_(&skips) {
    advance();  // 预读首条进 head_
  }

  [[nodiscard]] const Record* peek() override { return head_ ? &*head_ : nullptr; }

  void advance() override {
    head_.reset();
    while (std::getline(f_, line_)) {
      chomp_cr(line_);
      if (line_.empty()) continue;
      detail::split_csv(line_, fields_, owned_);
      if (auto rec = build_from_fields()) { head_ = std::move(*rec); return; }
    }
  }

private:
  // 切分后的字段 → Record;跳过(已按原因归账)返回 nullopt。视图随后即编码,无悬垂。
  [[nodiscard]] std::optional<Record> build_from_fields() {
    if (fields_.size() != n_cols_) { skips_->add(SkipReason::Malformed); return std::nullopt; }
    const auto ts = detail::to_i64(fields_[cols_.ts]);
    if (!ts) { skips_->add(SkipReason::BadTimestamp); return std::nullopt; }
    const auto sym = fields_[cols_.symbol];
    std::expected<Record, SkipReason> rec = std::unexpected(SkipReason::BadField);  // 默认:side 坏
    if (kind_ == Kind::Book) {
      const auto uid = detail::to_u64(fields_[cols_.update_id]);  // 交易所更新序号(u64,from_chars 拒负数)
      if (!uid) { skips_->add(SkipReason::BadField); return std::nullopt; }
      for (std::size_t k = 0; k < cols_.depth; ++k) {
        bpx_[k] = fields_[cols_.bid_px[k]];  bqty_[k] = fields_[cols_.bid_qty[k]];
        apx_[k] = fields_[cols_.ask_px[k]];  aqty_[k] = fields_[cols_.ask_qty[k]];
      }
      rec = make_book_record(*ts, *uid, sym, std::span(bpx_).first(cols_.depth),
                             std::span(bqty_).first(cols_.depth), std::span(apx_).first(cols_.depth),
                             std::span(aqty_).first(cols_.depth));
    } else if (const auto side = detail::to_i64(fields_[cols_.side])) {
      rec = make_trade_record(*ts, sym, *side, fields_[cols_.px], fields_[cols_.qty]);
    }
    if (!rec) { account_build_failure(*skips_, rec.error(), sym); return std::nullopt; }
    return *rec;
  }

  std::ifstream                 f_;
  Kind                          kind_;
  CsvCols                       cols_;
  std::size_t                   n_cols_;
  SkipStats*                    skips_;
  std::string                   line_;    // 逐 advance 复用
  std::vector<std::string_view> fields_;  // 切分结果(复用)
  std::vector<std::string>      owned_;   // 仅含引号行用(复用)
  std::optional<Record>         head_;
  std::array<std::string_view, kMaxDepth> bpx_, bqty_, apx_, aqty_;  // 逐行复用
};

// 开一个 CSV 文件 → 流式 Source:读+解析表头(eager 快速失败),再构造源。
// book 档数自动识别(1 或 5,残缺/>5 拒整文件,绝不截断);坏/未知行在消费(advance)时归账到 skips。
[[nodiscard]] inline Result<std::unique_ptr<Source>> load_csv_source(const std::string& path,
                                                                     Kind kind, SkipStats& skips) {
  std::ifstream f(path);
  if (!f) return std::unexpected(Error::FileOpen);

  std::string header;
  if (!std::getline(f, header)) return std::unexpected(Error::CsvSchema);  // 空文件 → 无表头
  chomp_cr(header);
  std::vector<std::string_view> hviews;
  std::vector<std::string>      howned;
  detail::split_csv(header, hviews, howned);
  std::vector<std::string> names;  // trim 后的列名(owning,源构造后表头串即弃)
  names.reserve(hviews.size());
  for (const auto v : hviews) names.emplace_back(detail::trim(v));

  auto cols = resolve_csv_cols(names, kind);
  if (!cols) {
    if (cols.error() == Error::BookDepthUnsupported) {  // 自描述:数出实际档数 + 告知支持范围
      const auto idx = [&](std::string_view n) {
        for (const auto& nm : names) if (nm == n) return true;
        return false;
      };
      const std::size_t got = highest_book_level([&](std::size_t k) { return idx("bid_px_" + std::to_string(k)); }) + 1;
      spdlog::error("csv '{}': book 档数 {} 不受支持 → 跳过该文件。mdreplay 只接受 1 档(BBO,列 bid_px/...)"
                    "或 5 档(列 ..._1.._4),不截断、不补齐。", path, got);
    }
    return std::unexpected(cols.error());
  }

  spdlog::debug("csv '{}': 流式载入, {} 档", path, cols->depth);
  return std::unique_ptr<Source>{
      std::make_unique<StreamingCsvSource>(std::move(f), kind, *cols, names.size(), skips)};
}

}  // namespace mdreplay
