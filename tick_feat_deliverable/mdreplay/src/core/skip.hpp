#pragma once
// skip.hpp — 跳过记录的分原因统计 + 未知符号采样。
//
// 回放器以「保真」为生命:把"坏行/缺列/未知符号/数值非法/scale 溢出"从单一计数拆成可定位的分类,
// 并在结束时 WARN 出去重的未知符号——指引用户「哪些符号不在 gconf subset、该去 symbols.h 补」。
// 未知符号是最常见的静默吞噬源(subset 仅 29 个),单独采样输出。

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace mdreplay {

enum class SkipReason : std::uint8_t {
  Malformed,      // 行结构坏:csv 列数与表头不符 / json 行非对象或解析失败
  BadTimestamp,   // ts 非整数
  BadField,       // 其它必需字段非法(如 trade side 非 0/1,json 字段类型错)
  UnknownSymbol,  // 符号不在 gconf subset
  BadNumber,      // 价/量非十进制数
  ScaleOverflow,  // 价/量 ×10^scale 越 u32 上限
  kCount
};

[[nodiscard]] constexpr std::string_view skip_reason_name(SkipReason r) noexcept {
  switch (r) {
    case SkipReason::Malformed:     return "malformed";
    case SkipReason::BadTimestamp:  return "bad_ts";
    case SkipReason::BadField:      return "bad_field";
    case SkipReason::UnknownSymbol: return "unknown_symbol";
    case SkipReason::BadNumber:     return "bad_number";
    case SkipReason::ScaleOverflow: return "scale_overflow";
    case SkipReason::kCount:        return "?";
  }
  return "?";
}

// 跨所有输入文件累计的跳过统计(main 持一份,贯穿 load → 汇总)。
class SkipStats {
public:
  void add(SkipReason r) noexcept {
    ++counts_[static_cast<std::size_t>(r)];
    ++total_;
  }

  // 未知符号:计数 + 采样去重名字(上限防爆内存;超限只计数不再采样)。
  void add_unknown(std::string_view sym) {
    add(SkipReason::UnknownSymbol);
    if (unknown_syms_.size() < kMaxUnknownSamples) unknown_syms_.emplace(sym);
  }

  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t count(SkipReason r) const noexcept {
    return counts_[static_cast<std::size_t>(r)];
  }

  // 结束汇总:INFO 分类明细 + WARN 去重未知符号(指引补 symbols.h)。total==0 时静默。
  void log_summary() const {
    if (total_ == 0) return;
    std::string br;
    for (std::size_t i = 0; i < static_cast<std::size_t>(SkipReason::kCount); ++i)
      if (counts_[i]) {
        br += ' ';
        br += skip_reason_name(static_cast<SkipReason>(i));
        br += '=';
        br += std::to_string(counts_[i]);
      }
    spdlog::info("skipped {} rows:{}", total_, br);

    const auto n_unknown = count(SkipReason::UnknownSymbol);
    if (n_unknown > 0) {
      std::string list;
      for (const auto& s : unknown_syms_) {
        if (!list.empty()) list += ", ";
        list += s;
      }
      spdlog::warn("{} rows skipped: symbol not in gconf subset ({} distinct sampled{}): {}",
                   n_unknown, unknown_syms_.size(),
                   unknown_syms_.size() >= kMaxUnknownSamples ? ", capped" : "", list);
    }
  }

private:
  static constexpr std::size_t kMaxUnknownSamples = 64;
  std::array<std::uint64_t, static_cast<std::size_t>(SkipReason::kCount)> counts_{};
  std::uint64_t         total_{0};
  std::set<std::string> unknown_syms_;
};

// 记录构建失败的归账:UnknownSymbol 走采样,其余按原因计数。csv/json 两路共用。
inline void account_build_failure(SkipStats& skips, SkipReason r, std::string_view sym) {
  if (r == SkipReason::UnknownSymbol) skips.add_unknown(sym);
  else skips.add(r);
}

}  // namespace mdreplay
