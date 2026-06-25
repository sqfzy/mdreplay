#pragma once
// gid.hpp — symbol 名 → gconf 符号 id(用 gconf 共享符号层,非自带)。
// book 段按 **LID** 索引 slot[lid];trade 段 payload 用 **GID**。v1.2.2 共享层里 LID 与 GID 当前为
// 恒等映射(kLocalToGid identity,27 symbol 跨 venue 同序),故两者同值=kGidNames 下标。
// 非 subset 符号返回 nullopt,由调用方计数跳过。

#include <cstdint>
#include <optional>
#include <string_view>

#include <gconf/symbol/symbol_idx.h>

namespace mdreplay {

// symbol → GID(= kGidNames 下标)。trade 段 TradePayload.global_symbol_id 用它。
[[nodiscard]] inline std::optional<std::uint16_t> gid_of(std::string_view name) {
  for (std::uint16_t g = 0; g < gconf::sym::N_GIDS; ++g)
    if (gconf::sym::kGidNames[g] == name) return g;
  return std::nullopt;
}

// symbol → LID(book 段 slot 下标)。当前 gconf kLocalToGid 恒等 → lid == gid。
[[nodiscard]] inline std::optional<std::uint16_t> lid_of(std::string_view name) {
  return gid_of(name);
}

}  // namespace mdreplay
