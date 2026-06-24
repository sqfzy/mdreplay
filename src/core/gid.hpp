#pragma once
// gid.hpp — symbol 名 → gconf global_symbol_id(用 gconf 契约的符号表,非自带)。
// 非 subset 符号返回 nullopt,由调用方计数跳过。

#include <cstdint>
#include <optional>
#include <string_view>

#include <gconf/domain/symbols.h>

namespace mdreplay {

[[nodiscard]] inline std::optional<std::uint16_t> gid_of(std::string_view name) {
  for (std::uint16_t g = 0; g < gconf::sym::N_GLOBAL_SYMBOL_IDS; ++g)
    if (gconf::sym::kNames[g] == name) return g;
  return std::nullopt;
}

}  // namespace mdreplay
