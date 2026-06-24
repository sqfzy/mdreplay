#pragma once
// fixed.hpp — 十进制字符串 → 定点(uint32 mantissa ×10^scale)。scale 取数据天然精度,不规定。
//
// 一组同类值(如 BBO 的 bid/ask 价)编码到「公共 scale」= 组内最大小数位(对齐 BoardSlot 单一
// price_scale/qty_scale)。整组在该 scale 下溢出 u32 时自动降 scale(丢最末小数、保住记录);
// 仅当整数部分本身 > u32(币价 > 42 亿,不可能)才判 ScaleOverflow。零浮点,精确。

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace mdreplay {

inline constexpr std::uint32_t kU32Max = 4294967295u;

// 去尾零后的小数位数;无 '.' → 0。
[[nodiscard]] inline int decimals_of(std::string_view s) {
  const auto dot = s.find('.');
  if (dot == std::string_view::npos) return 0;
  std::string_view frac = s.substr(dot + 1);
  while (!frac.empty() && frac.back() == '0') frac.remove_suffix(1);
  return static_cast<int>(frac.size());
}

// 在给定 scale 下把 "INT.FRAC" 编码为整数(uint64 便于检测越界);负/空/非数字 → nullopt。
// 超过 scale 的小数位截断丢弃。
[[nodiscard]] inline std::optional<std::uint64_t> mantissa_at(std::string_view s, int scale) {
  if (s.empty() || s.front() == '-') return std::nullopt;
  const auto dot = s.find('.');
  std::string_view int_part = (dot == std::string_view::npos) ? s : s.substr(0, dot);
  std::string_view frac     = (dot == std::string_view::npos) ? std::string_view{} : s.substr(dot + 1);
  if (int_part.empty()) int_part = "0";

  std::uint64_t m = 0;
  const auto push = [&](char c) -> bool {
    if (c < '0' || c > '9') return false;
    m = m * 10 + static_cast<std::uint64_t>(c - '0');
    return true;
  };
  for (const char c : int_part)
    if (!push(c)) return std::nullopt;
  for (int i = 0; i < scale; ++i)
    if (!push(i < static_cast<int>(frac.size()) ? frac[static_cast<std::size_t>(i)] : '0'))
      return std::nullopt;
  return m;
}

// 把同类十进制串组编码到公共 scale,写回 out(顺序对应 in)。返回公共 scale,或错误。
[[nodiscard]] inline Result<std::uint8_t> encode_group(std::span<const std::string_view> in,
                                                       std::span<std::uint32_t> out) {
  int scale = 0;
  for (const auto s : in) scale = std::max(scale, decimals_of(s));
  for (; scale >= 0; --scale) {
    bool fits = true;
    for (std::size_t i = 0; i < in.size(); ++i) {
      const auto m = mantissa_at(in[i], scale);
      if (!m) return std::unexpected(Error::CsvParse);   // 非法数值
      if (*m > kU32Max) { fits = false; break; }          // 该 scale 溢出 → 降一档重编
      out[i] = static_cast<std::uint32_t>(*m);
    }
    if (fits) return static_cast<std::uint8_t>(scale);
  }
  return std::unexpected(Error::ScaleOverflow);            // 整数部分都越界(不可能的高价)
}

// 定点 → 十进制字符串(编码的逆;mantissa=6884,scale=2 → "68.84")。供 csv/json 输出还原精度。
[[nodiscard]] inline std::string to_decimal(std::uint32_t mantissa, std::uint8_t scale) {
  std::string s = std::to_string(mantissa);
  if (scale == 0) return s;
  if (s.size() <= scale) s.insert(0, scale - s.size() + 1, '0');  // 补前导零保证有整数位
  s.insert(s.size() - scale, ".");
  return s;
}

}  // namespace mdreplay
