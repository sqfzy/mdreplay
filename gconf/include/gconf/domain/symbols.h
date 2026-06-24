#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

// Symbol identity for the bybit producer.
//
// TWO id spaces (see also the design note in the bins):
//   global symbol id  — stable, append-only, covers the FULL universe incl
//                      BTC/ETH. Used as the SHM/ABI index (market board,
//                      syminfo table, board slot global_symbol_id). === bn_fu's global symbol id space.
//   local symbol id   — dense 0..N_SYMBOLS-1 over the actively-traded subset27
//                      (no BTC/ETH). Used as the hot per-symbol index
//                      (event.local_symbol_id, mirror local array). Identical
//                      numbering to bn's subset27.
//
// `match()` parses a BYBIT V5 frame's symbol bytes → local symbol id (venue-specific
// parser; its local symbol id output is shared with bn). `kLocalToGlobalSymbolId[local_symbol_id]` → bn global symbol id.

namespace gconf::sym {

// 编译期 Pack8: symbol name 低 8 字节按 ASCII 小端打包。
consteval std::uint64_t make_pack8(std::string_view s) noexcept {
  std::uint64_t v = 0;
  const std::size_t L = s.size();
  const std::size_t lim = (L < 8) ? L : 8;
  for (std::size_t i = 0; i < lim; ++i)
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[i])) << (8 * i);
  return v;
}

// ── global symbol id = bn_fu space: 纯字母序, BTC=8 / ETH=13 / SOL=21 ─────────
enum GlobalSymbolId : std::uint16_t {
  AAVEUSDT_GLOBAL_SYMBOL_ID = 0, ADAUSDT_GLOBAL_SYMBOL_ID, APTUSDT_GLOBAL_SYMBOL_ID, ARBUSDT_GLOBAL_SYMBOL_ID, AVAXUSDT_GLOBAL_SYMBOL_ID, AXSUSDT_GLOBAL_SYMBOL_ID,
  BCHUSDT_GLOBAL_SYMBOL_ID, BNBUSDT_GLOBAL_SYMBOL_ID, BTCUSDT_GLOBAL_SYMBOL_ID, CRVUSDT_GLOBAL_SYMBOL_ID, DOGEUSDT_GLOBAL_SYMBOL_ID, DOTUSDT_GLOBAL_SYMBOL_ID,
  ENSUSDT_GLOBAL_SYMBOL_ID, ETHUSDT_GLOBAL_SYMBOL_ID, HBARUSDT_GLOBAL_SYMBOL_ID, LTCUSDT_GLOBAL_SYMBOL_ID, OPUSDT_GLOBAL_SYMBOL_ID, ORDIUSDT_GLOBAL_SYMBOL_ID,
  PEOPLEUSDT_GLOBAL_SYMBOL_ID, PNUTUSDT_GLOBAL_SYMBOL_ID, SANDUSDT_GLOBAL_SYMBOL_ID, SOLUSDT_GLOBAL_SYMBOL_ID, SUIUSDT_GLOBAL_SYMBOL_ID, TIAUSDT_GLOBAL_SYMBOL_ID,
  TRUMPUSDT_GLOBAL_SYMBOL_ID, UNIUSDT_GLOBAL_SYMBOL_ID, WLDUSDT_GLOBAL_SYMBOL_ID, XLMUSDT_GLOBAL_SYMBOL_ID, XRPUSDT_GLOBAL_SYMBOL_ID,
  N_GLOBAL_SYMBOL_IDS
};
// global symbol id 是落进 SHM 的持久语义(board slot global_symbol_id / syminfo 索引)。重排或非尾部插入会让所有
// 已 mmap 数据语义整体平移 → 跨版本错位。锚定关键 global symbol id,任何重排即编译失败(#87)。
static_assert(BTCUSDT_GLOBAL_SYMBOL_ID == 8 && ETHUSDT_GLOBAL_SYMBOL_ID == 13 && SOLUSDT_GLOBAL_SYMBOL_ID == 21 && N_GLOBAL_SYMBOL_IDS == 29,
              "global symbol id layout is a persisted SHM contract — append only at the tail");

inline constexpr std::array<std::string_view, N_GLOBAL_SYMBOL_IDS> kNames = {
    "AAVEUSDT", "ADAUSDT",  "APTUSDT",  "ARBUSDT",  "AVAXUSDT",  "AXSUSDT",
    "BCHUSDT",  "BNBUSDT",  "BTCUSDT",  "CRVUSDT",  "DOGEUSDT",  "DOTUSDT",
    "ENSUSDT",  "ETHUSDT",  "HBARUSDT", "LTCUSDT",  "OPUSDT",    "ORDIUSDT",
    "PEOPLEUSDT","PNUTUSDT","SANDUSDT", "SOLUSDT",  "SUIUSDT",   "TIAUSDT",
    "TRUMPUSDT","UNIUSDT",  "WLDUSDT",  "XLMUSDT",  "XRPUSDT",
};
static_assert(kNames.size() == N_GLOBAL_SYMBOL_IDS, "kNames size must equal N_GLOBAL_SYMBOL_IDS");

constexpr std::string_view global_symbol_id_name(int global_symbol_id) noexcept {
  return (global_symbol_id >= 0 && global_symbol_id < N_GLOBAL_SYMBOL_IDS) ? kNames[global_symbol_id] : std::string_view{};
}

// ── local symbol id (subset27 local id) = dense hot index (无 BTC/ETH; 与 bn 同序) ───────
enum LocalSymbolId : std::uint16_t {
  AAVEUSDT_LOCAL_SYMBOL_ID = 0, ADAUSDT_LOCAL_SYMBOL_ID, APTUSDT_LOCAL_SYMBOL_ID, ARBUSDT_LOCAL_SYMBOL_ID, AVAXUSDT_LOCAL_SYMBOL_ID, AXSUSDT_LOCAL_SYMBOL_ID,
  BCHUSDT_LOCAL_SYMBOL_ID, BNBUSDT_LOCAL_SYMBOL_ID, CRVUSDT_LOCAL_SYMBOL_ID, DOGEUSDT_LOCAL_SYMBOL_ID, DOTUSDT_LOCAL_SYMBOL_ID, ENSUSDT_LOCAL_SYMBOL_ID,
  HBARUSDT_LOCAL_SYMBOL_ID, LTCUSDT_LOCAL_SYMBOL_ID, OPUSDT_LOCAL_SYMBOL_ID, ORDIUSDT_LOCAL_SYMBOL_ID, PEOPLEUSDT_LOCAL_SYMBOL_ID, PNUTUSDT_LOCAL_SYMBOL_ID,
  SANDUSDT_LOCAL_SYMBOL_ID, SOLUSDT_LOCAL_SYMBOL_ID, SUIUSDT_LOCAL_SYMBOL_ID, TIAUSDT_LOCAL_SYMBOL_ID, TRUMPUSDT_LOCAL_SYMBOL_ID, UNIUSDT_LOCAL_SYMBOL_ID,
  WLDUSDT_LOCAL_SYMBOL_ID, XLMUSDT_LOCAL_SYMBOL_ID, XRPUSDT_LOCAL_SYMBOL_ID,
  N_SYMBOLS
};
// local symbol id(subset27)与 bn 同序,是事件/订单热路径索引。重排会与 bn 失配 → 跨进程串台(#87)。
static_assert(SOLUSDT_LOCAL_SYMBOL_ID == 19 && N_SYMBOLS == 27, "local symbol id subset27 layout is shared with bn — append-only");

// local symbol id → global symbol id (bn kLocalToGlobalSymbolId: 跳过 BTCUSDT_GLOBAL_SYMBOL_ID / ETHUSDT_GLOBAL_SYMBOL_ID)。顺序与 LocalSymbolId 一致。
inline constexpr std::array<int, N_SYMBOLS> kLocalToGlobalSymbolId = {
    AAVEUSDT_GLOBAL_SYMBOL_ID, ADAUSDT_GLOBAL_SYMBOL_ID, APTUSDT_GLOBAL_SYMBOL_ID, ARBUSDT_GLOBAL_SYMBOL_ID, AVAXUSDT_GLOBAL_SYMBOL_ID, AXSUSDT_GLOBAL_SYMBOL_ID,
    BCHUSDT_GLOBAL_SYMBOL_ID,  BNBUSDT_GLOBAL_SYMBOL_ID, CRVUSDT_GLOBAL_SYMBOL_ID, DOGEUSDT_GLOBAL_SYMBOL_ID, DOTUSDT_GLOBAL_SYMBOL_ID, ENSUSDT_GLOBAL_SYMBOL_ID,
    HBARUSDT_GLOBAL_SYMBOL_ID, LTCUSDT_GLOBAL_SYMBOL_ID, OPUSDT_GLOBAL_SYMBOL_ID,  ORDIUSDT_GLOBAL_SYMBOL_ID, PEOPLEUSDT_GLOBAL_SYMBOL_ID, PNUTUSDT_GLOBAL_SYMBOL_ID,
    SANDUSDT_GLOBAL_SYMBOL_ID, SOLUSDT_GLOBAL_SYMBOL_ID, SUIUSDT_GLOBAL_SYMBOL_ID, TIAUSDT_GLOBAL_SYMBOL_ID, TRUMPUSDT_GLOBAL_SYMBOL_ID, UNIUSDT_GLOBAL_SYMBOL_ID,
    WLDUSDT_GLOBAL_SYMBOL_ID,  XLMUSDT_GLOBAL_SYMBOL_ID, XRPUSDT_GLOBAL_SYMBOL_ID,
};
static_assert(kLocalToGlobalSymbolId.size() == static_cast<std::size_t>(N_SYMBOLS), "kLocalToGlobalSymbolId size must equal N_SYMBOLS");

constexpr int to_global_symbol_id(std::uint16_t local_symbol_id) noexcept { return (local_symbol_id < N_SYMBOLS) ? kLocalToGlobalSymbolId[local_symbol_id] : -1; }
constexpr std::string_view local_symbol_id_name(std::uint16_t local_symbol_id) noexcept {
  const int global_symbol_id = to_global_symbol_id(local_symbol_id);
  return (global_symbol_id >= 0) ? kNames[global_symbol_id] : std::string_view{};
}

using SymbolMatchFn = std::uint16_t (*)(const std::uint8_t *p, std::uint16_t s_begin, std::uint16_t total_len, std::uint16_t &s_end) noexcept;

// ── Pack8 常量 (subset27 名字; match 用 u64 比较) ────────────────────────────
inline constexpr std::uint64_t PACK_AAVEUSDT = make_pack8("AAVEUSDT");
inline constexpr std::uint64_t PACK_ADAUSDT = make_pack8("ADAUSDT");
inline constexpr std::uint64_t PACK_APTUSDT = make_pack8("APTUSDT");
inline constexpr std::uint64_t PACK_ARBUSDT = make_pack8("ARBUSDT");
inline constexpr std::uint64_t PACK_AVAXUSDT = make_pack8("AVAXUSDT");
inline constexpr std::uint64_t PACK_AXSUSDT = make_pack8("AXSUSDT");
inline constexpr std::uint64_t PACK_BCHUSDT = make_pack8("BCHUSDT");
inline constexpr std::uint64_t PACK_BNBUSDT = make_pack8("BNBUSDT");
inline constexpr std::uint64_t PACK_CRVUSDT = make_pack8("CRVUSDT");
inline constexpr std::uint64_t PACK_DOGEUSDT = make_pack8("DOGEUSDT");
inline constexpr std::uint64_t PACK_DOTUSDT = make_pack8("DOTUSDT");
inline constexpr std::uint64_t PACK_ENSUSDT = make_pack8("ENSUSDT");
inline constexpr std::uint64_t PACK_HBARUSDT = make_pack8("HBARUSDT");
inline constexpr std::uint64_t PACK_LTCUSDT = make_pack8("LTCUSDT");
inline constexpr std::uint64_t PACK_OPUSDT = make_pack8("OPUSDT");
inline constexpr std::uint64_t PACK_ORDIUSDT = make_pack8("ORDIUSDT");
inline constexpr std::uint64_t PACK_PEOPLEUSDT = make_pack8("PEOPLEUSDT");
inline constexpr std::uint64_t PACK_PNUTUSDT = make_pack8("PNUTUSDT");
inline constexpr std::uint64_t PACK_SANDUSDT = make_pack8("SANDUSDT");
inline constexpr std::uint64_t PACK_SOLUSDT = make_pack8("SOLUSDT");
inline constexpr std::uint64_t PACK_SUIUSDT = make_pack8("SUIUSDT");
inline constexpr std::uint64_t PACK_TIAUSDT = make_pack8("TIAUSDT");
inline constexpr std::uint64_t PACK_TRUMPUSDT = make_pack8("TRUMPUSDT");
inline constexpr std::uint64_t PACK_UNIUSDT = make_pack8("UNIUSDT");
inline constexpr std::uint64_t PACK_WLDUSDT = make_pack8("WLDUSDT");
inline constexpr std::uint64_t PACK_XLMUSDT = make_pack8("XLMUSDT");
inline constexpr std::uint64_t PACK_XRPUSDT = make_pack8("XRPUSDT");

// 从 payload 的 s_begin 处提取 symbol → 返回 local symbol id; s_end 回填闭合 '"' 偏移。
// BYBIT V5 帧解析专用 (从 s_begin 处 load 8 字节, 帧 >100B 天然满足)。
// 非 subset27 symbol 返回 0xFFFF。
inline std::uint16_t match(const std::uint8_t *p, std::uint16_t s_begin, std::uint16_t total_len, std::uint16_t &s_end) noexcept {
  s_end = s_begin;
  while (s_end < total_len && p[s_end] != '"')
    ++s_end;
  const int L = s_end - s_begin;

  std::uint64_t v8;
  std::memcpy(&v8, p + s_begin, 8);

  switch (L) {
  case 6: {
    const std::uint64_t k = v8 & 0x0000FFFFFFFFFFFFULL;
    return (k == PACK_OPUSDT) ? static_cast<std::uint16_t>(OPUSDT_LOCAL_SYMBOL_ID) : static_cast<std::uint16_t>(-1);
  }
  case 7: {
    const std::uint64_t k = v8 & 0x00FFFFFFFFFFFFFFULL;
    switch (k) {
    case PACK_ADAUSDT:  return ADAUSDT_LOCAL_SYMBOL_ID;
    case PACK_APTUSDT:  return APTUSDT_LOCAL_SYMBOL_ID;
    case PACK_ARBUSDT:  return ARBUSDT_LOCAL_SYMBOL_ID;
    case PACK_AXSUSDT:  return AXSUSDT_LOCAL_SYMBOL_ID;
    case PACK_BCHUSDT:  return BCHUSDT_LOCAL_SYMBOL_ID;
    case PACK_BNBUSDT:  return BNBUSDT_LOCAL_SYMBOL_ID;
    case PACK_CRVUSDT:  return CRVUSDT_LOCAL_SYMBOL_ID;
    case PACK_DOTUSDT:  return DOTUSDT_LOCAL_SYMBOL_ID;
    case PACK_ENSUSDT:  return ENSUSDT_LOCAL_SYMBOL_ID;
    case PACK_LTCUSDT:  return LTCUSDT_LOCAL_SYMBOL_ID;
    case PACK_SOLUSDT:  return SOLUSDT_LOCAL_SYMBOL_ID;
    case PACK_SUIUSDT:  return SUIUSDT_LOCAL_SYMBOL_ID;
    case PACK_TIAUSDT:  return TIAUSDT_LOCAL_SYMBOL_ID;
    case PACK_UNIUSDT:  return UNIUSDT_LOCAL_SYMBOL_ID;
    case PACK_WLDUSDT:  return WLDUSDT_LOCAL_SYMBOL_ID;
    case PACK_XLMUSDT:  return XLMUSDT_LOCAL_SYMBOL_ID;
    case PACK_XRPUSDT:  return XRPUSDT_LOCAL_SYMBOL_ID;
    default:            return static_cast<std::uint16_t>(-1);
    }
  }
  case 8: {
    switch (v8) {
    case PACK_AAVEUSDT: return AAVEUSDT_LOCAL_SYMBOL_ID;
    case PACK_AVAXUSDT: return AVAXUSDT_LOCAL_SYMBOL_ID;
    case PACK_DOGEUSDT: return DOGEUSDT_LOCAL_SYMBOL_ID;
    case PACK_HBARUSDT: return HBARUSDT_LOCAL_SYMBOL_ID;
    case PACK_ORDIUSDT: return ORDIUSDT_LOCAL_SYMBOL_ID;
    case PACK_PNUTUSDT: return PNUTUSDT_LOCAL_SYMBOL_ID;
    case PACK_SANDUSDT: return SANDUSDT_LOCAL_SYMBOL_ID;
    default:            return static_cast<std::uint16_t>(-1);
    }
  }
  case 9:  // 前 8 字节 "TRUMPUSD"
    return (v8 == PACK_TRUMPUSDT) ? static_cast<std::uint16_t>(TRUMPUSDT_LOCAL_SYMBOL_ID) : static_cast<std::uint16_t>(-1);
  case 10: // 前 8 字节 "PEOPLEUS"
    return (v8 == PACK_PEOPLEUSDT) ? static_cast<std::uint16_t>(PEOPLEUSDT_LOCAL_SYMBOL_ID) : static_cast<std::uint16_t>(-1);
  default:
    return static_cast<std::uint16_t>(-1);
  }
}

} // namespace gconf::sym
