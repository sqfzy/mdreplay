#pragma once
#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

// Shared symbol identity (venue-agnostic).

namespace gconf::sym {

// ── GID 全局表，可以跨服务器传输 ─────────────────────
enum G_SYMBOL_IDX : std::uint16_t {
  AAVEUSDT_G = 0,
  ADAUSDT_G,
  APTUSDT_G,
  ARBUSDT_G,
  AVAXUSDT_G,
  AXSUSDT_G,
  BCHUSDT_G,
  BNBUSDT_G,
  CRVUSDT_G,
  DOGEUSDT_G,
  DOTUSDT_G,
  ENSUSDT_G,
  HBARUSDT_G,
  LTCUSDT_G,
  OPUSDT_G,
  ORDIUSDT_G,
  PEOPLEUSDT_G,
  PNUTUSDT_G,
  SANDUSDT_G,
  SOLUSDT_G,
  SUIUSDT_G,
  TIAUSDT_G,
  TRUMPUSDT_G,
  UNIUSDT_G,
  WLDUSDT_G,
  XLMUSDT_G,
  XRPUSDT_G,
  N_GIDS
};
// GID 是落进 SHM 的持久语义(board slot gid / syminfo 索引)。重排或非尾部插入会让所有
// 已 mmap 数据语义整体平移 → 跨版本错位。锚定关键 GID,任何重排即编译失败(#87)。
static_assert(SOLUSDT_G == 19 && N_GIDS == 27, "GID layout is a persisted SHM contract — append only at the tail");

// GID → 裸名(canonical 显示名,日志 / 工具用)。gconf内部命名,与交易所无关
inline constexpr std::array<std::string_view, N_GIDS> kGidNames = {
    "AAVEUSDT", "ADAUSDT", "APTUSDT", "ARBUSDT",  "AVAXUSDT",  "AXSUSDT", "BCHUSDT",  "BNBUSDT",    "CRVUSDT",
    "DOGEUSDT", "DOTUSDT", "ENSUSDT", "HBARUSDT", "LTCUSDT",   "OPUSDT",  "ORDIUSDT", "PEOPLEUSDT", "PNUTUSDT",
    "SANDUSDT", "SOLUSDT", "SUIUSDT", "TIAUSDT",  "TRUMPUSDT", "UNIUSDT", "WLDUSDT",  "XLMUSDT",    "XRPUSDT",
};
static_assert(kGidNames.size() == N_GIDS, "kGidNames size must equal N_GIDS");

// ── LID 服务器内部表，只在当前服务器生效 ──────────
enum L_SYMBOL_IDX : std::uint16_t {
  AAVEUSDT_L = 0,
  ADAUSDT_L,
  APTUSDT_L,
  ARBUSDT_L,
  AVAXUSDT_L,
  AXSUSDT_L,
  BCHUSDT_L,
  BNBUSDT_L,
  CRVUSDT_L,
  DOGEUSDT_L,
  DOTUSDT_L,
  ENSUSDT_L,
  HBARUSDT_L,
  LTCUSDT_L,
  OPUSDT_L,
  ORDIUSDT_L,
  PEOPLEUSDT_L,
  PNUTUSDT_L,
  SANDUSDT_L,
  SOLUSDT_L,
  SUIUSDT_L,
  TIAUSDT_L,
  TRUMPUSDT_L,
  UNIUSDT_L,
  WLDUSDT_L,
  XLMUSDT_L,
  XRPUSDT_L,
  N_SYMS
};
// LID 是事件/订单热路径索引,所有交易所共用同序。重排会跨进程串台(#87)。
static_assert(SOLUSDT_L == 19 && N_SYMS == 27, "LID layout is shared across venues — append-only");

// LID → GID。顺序与 L_SYMBOL_IDX 一致 (现为恒等映射)。
inline constexpr std::array<int, N_SYMS> kLocalToGid = {
    AAVEUSDT_G, ADAUSDT_G, APTUSDT_G, ARBUSDT_G,  AVAXUSDT_G,  AXSUSDT_G, BCHUSDT_G,  BNBUSDT_G,    CRVUSDT_G,
    DOGEUSDT_G, DOTUSDT_G, ENSUSDT_G, HBARUSDT_G, LTCUSDT_G,   OPUSDT_G,  ORDIUSDT_G, PEOPLEUSDT_G, PNUTUSDT_G,
    SANDUSDT_G, SOLUSDT_G, SUIUSDT_G, TIAUSDT_G,  TRUMPUSDT_G, UNIUSDT_G, WLDUSDT_G,  XLMUSDT_G,    XRPUSDT_G,
};
static_assert(kLocalToGid.size() == static_cast<std::size_t>(N_SYMS), "kLocalToGid size must equal N_SYMS");

constexpr int to_gid(std::uint16_t lid) noexcept { return (lid < N_SYMS) ? kLocalToGid[lid] : -1; }

// venue 的 match() 函数签名:从 payload 的 s_begin 处提取 symbol → 返回 LID,s_end 回填闭合 '"' 偏移。
// dispatcher / traits 用它做 NTTP(编译期绑定各 venue 的 match)。
using SymbolMatchFn = std::uint16_t (*)(const std::uint8_t *p, std::uint16_t s_begin, std::uint16_t total_len, std::uint16_t &s_end) noexcept;

// ── venue 接入契约 ───────────────────────────────────────────
// 接新交易所需在其 Index 里实现的全部必选项,违反即编译失败(各 venue 以 static_assert(VenueIndex<Index>) 自检):
//   kNames     GID→wire 名表 (std::array<string_view, N_GIDS>)
//   get_name   LID → wire 名字
//   match      payload → LID 解析 (SymbolMatchFn 签名)
template <class V>
concept VenueIndex =
    std::same_as<std::remove_cvref_t<decltype(V::kNames)>, std::array<std::string_view, N_GIDS>>
    && requires(std::uint16_t lid, const std::uint8_t *p, std::uint16_t s_begin, std::uint16_t total_len, std::uint16_t &s_end) {
         { V::get_name(lid) } -> std::same_as<std::string_view>;
         { V::match(p, s_begin, total_len, s_end) } -> std::same_as<std::uint16_t>;
       };

} // namespace gconf::sym
