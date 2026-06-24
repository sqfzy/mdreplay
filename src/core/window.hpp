#pragma once
// window.hpp — 回放时间窗的「无界」哨兵。merge 与 config 共用,避免 config 仅为常量而依赖归并引擎。

#include <cstdint>
#include <limits>

namespace mdreplay {

inline constexpr std::int64_t kNoStart = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t kNoEnd   = std::numeric_limits<std::int64_t>::max();

}  // namespace mdreplay
