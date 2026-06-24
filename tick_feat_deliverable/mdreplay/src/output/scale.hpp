#pragma once
// scale.hpp — double 原值 → uint32 ×10^scale 编码(落进 gconf board/trade 的定点字段)。
//
// gconf board #81:value×10^scale 不得溢出 u32(~4.29e9)。溢出/非法值返回 ScaleOverflow,
// 调用方按口径 WARN+计数+跳行,不中断回放。

#include <cmath>
#include <cstdint>

#include "error.hpp"

namespace mdreplay {

inline constexpr std::uint32_t kU32Max = 4294967295u;

// 10^scale(scale 较小,直接整数累乘成 double;≤10^15 在 double 内精确)。
[[nodiscard]] inline double pow10(std::uint8_t scale) noexcept {
  double f = 1.0;
  for (std::uint8_t i = 0; i < scale; ++i) f *= 10.0;
  return f;
}

// 四舍五入到定点;负/非有限/越界 → ScaleOverflow。
[[nodiscard]] inline Result<std::uint32_t> to_scaled(double value, std::uint8_t scale) noexcept {
  if (!std::isfinite(value) || value < 0.0) return std::unexpected(Error::ScaleOverflow);
  const double scaled = std::nearbyint(value * pow10(scale));
  if (scaled > static_cast<double>(kU32Max)) return std::unexpected(Error::ScaleOverflow);
  return static_cast<std::uint32_t>(scaled);
}

}  // namespace mdreplay
