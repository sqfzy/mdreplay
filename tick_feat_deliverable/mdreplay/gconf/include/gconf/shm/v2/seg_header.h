#pragma once
// gconf/shm/v2/seg_header.h — v2 SHM 段的版本化自描述头(每段第一个 cache line)。
//
// 范式:SBE message header(blockLength/templateId/schemaId/version)+ Aeron log-buffer
// 元数据。attach 时用 seg_check() 把段头与本进程编译期常量逐字段比对,把 v1 仅比 sizeof(Seg)
// 的弱校验升级为结构化自校验。
//
// 【纯契约·零三方依赖】:本头只依赖标准库(可被异构语言绑定 / 外部团队 / 单测直接 include)。
// seg_check() 返回 SegError(不打印);要带 spdlog 日志的便捷包装见 seg_validate.h(非纯契约)。
//
// 布局铁律(全部 static_assert 锁死):零裸指针 / POD / sizeof==64 / alignof==64 / 关键字段 offset。

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace gconf::shm::v2 {

// 契约假设小端集群(x86-64 dev + aarch64-LE prod 均小端)。跨字节序的 SHM/bridge 传输不支持:
// 大端对端写出的 magic 会被本端读成字节反转值 → seg_check 报 BadMagic(运行期兜底);本断言在
// 编译期就拒绝把本契约编进大端目标。
static_assert(std::endian::native == std::endian::little,
              "gconf SHM contract is little-endian only (cross-endian SHM/bridge unsupported)");

inline constexpr std::uint32_t kSegMagic   = 0x47434632u;  // "GCF2"
inline constexpr std::uint16_t kLayoutVer  = 2;

enum class SegKind : std::uint16_t { Board = 1, BcastRing = 2, SpscQueue = 3 };

// 段头:64B,独占一 cache line。放在每个 v2 段的最前面。
struct alignas(64) SegHeader {
  std::uint32_t magic;          // = kSegMagic
  std::uint16_t layout_version; // = kLayoutVer
  SegKind       kind;           // Board / BcastRing / SpscQueue
  std::uint32_t entry_size;     // sizeof(Entry / Slot)
  std::uint32_t capacity;       // ring/queue: 2 的幂;board: N_GLOBAL_SYMBOL_IDS
  std::uint64_t schema_hash;    // 字段布局的编译期 FNV(防错位;先告警)
  std::uint64_t created_ns;     // shm_init 建段时刻(诊断用,可为 0)
  std::uint8_t  _rsvd[32];      // 同版本内加字段可填此处,sizeof 不变
};
static_assert(sizeof(SegHeader) == 64, "SegHeader must be exactly one cache line");
static_assert(alignof(SegHeader) == 64, "SegHeader must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<SegHeader>, "SegHeader must be POD (SHM contract)");
// 段头字段偏移是 attach 校验的契约:任一 bin padding 不同会让 seg_check 读错字段(#82)。
static_assert(offsetof(SegHeader, magic) == 0 && offsetof(SegHeader, entry_size) == 8 &&
              offsetof(SegHeader, capacity) == 12 && offsetof(SegHeader, schema_hash) == 16,
              "SegHeader field offsets are part of the attach contract");

// 编译期 FNV-1a:对"字段布局描述串"求哈希,放进 schema_hash 防字段错位。
// 改了字段布局就改描述串 → 哈希变 → attach 时告警(提示双方版本不一致)。
constexpr std::uint64_t schema_fnv(const char* s) noexcept {
  std::uint64_t h = 1469598103934665603ull;
  for (; *s; ++s) h = (h ^ static_cast<std::uint8_t>(*s)) * 1099511628211ull;
  return h;
}

// shm_init 建段后调用:写好段头。
inline void seg_init(SegHeader& h, SegKind kind, std::uint32_t entry_size,
                     std::uint32_t capacity, std::uint64_t schema_hash, std::uint64_t now_ns) noexcept {
  h.magic = kSegMagic; h.layout_version = kLayoutVer; h.kind = kind;
  h.entry_size = entry_size; h.capacity = capacity; h.schema_hash = schema_hash; h.created_ns = now_ns;
}

// attach 校验结果。BadMagic/BadVersion/BadKind/AbiMismatch = 致命(布局根本对不上);
// SchemaDrift = 字段级演进期可容忍(调用方决定:通常告警后继续)。
enum class SegError : std::uint8_t { Ok, BadMagic, BadVersion, BadKind, AbiMismatch, SchemaDrift };

[[nodiscard]] constexpr std::string_view seg_error_str(SegError e) noexcept {
  switch (e) {
    case SegError::Ok:          return "ok";
    case SegError::BadMagic:    return "bad magic (wrong/uninit segment, or cross-endian peer)";
    case SegError::BadVersion:  return "layout_version mismatch";
    case SegError::BadKind:     return "wrong segment kind";
    case SegError::AbiMismatch: return "entry_size/capacity mismatch (rebuild shm_init?)";
    case SegError::SchemaDrift: return "schema_hash drift (field layout changed)";
  }
  return "?";
}

// attach 方:把段头与本进程编译期常量逐字段比对(纯函数,不打印,不分配)。
// 返回第一处不符;全符 → Ok。SchemaDrift 是最弱级(布局尺寸都对,只是描述串哈希不同)。
[[nodiscard]] inline SegError seg_check(const SegHeader& h, SegKind kind, std::uint32_t entry_size,
                                        std::uint32_t capacity, std::uint64_t schema_hash) noexcept {
  if (h.magic != kSegMagic)                                  return SegError::BadMagic;
  if (h.layout_version != kLayoutVer)                        return SegError::BadVersion;
  if (h.kind != kind)                                        return SegError::BadKind;
  if (h.entry_size != entry_size || h.capacity != capacity)  return SegError::AbiMismatch;
  if (h.schema_hash != schema_hash)                          return SegError::SchemaDrift;
  return SegError::Ok;
}

}  // namespace gconf::shm::v2
