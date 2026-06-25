#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

//  跨进程 SHM 的根本困境:生产者和消费者各自独立编译、独立部署,运行时无法握手协商。消费者 mmap 上来一块 /dev/shm 内存,它怎么知道:
//   1. 这块内存到底是不是一个 gconf v2 段?(可能挂错名、是上次没清的脏段、甚至是大端对端写的)
//   2. 它的 ABI(entry 大小、容量)和我编译时假设的一致吗?(对面可能用旧版重建过)
//   3. 字段布局有没有悄悄漂移?(sizeof 相同但字段顺序变了)

namespace gconf::shm::v2 {

// 契约假设小端集群(x86-64 dev + aarch64-LE prod 均小端)。跨字节序的 SHM/bridge 传输不支持:
// 大端对端写出的 magic 会被本端读成字节反转值 → seg_check 报 BadMagic(运行期兜底);本断言在
// 编译期就拒绝把本契约编进大端目标。
static_assert(std::endian::native == std::endian::little, "gconf SHM contract is little-endian only (cross-endian SHM/bridge unsupported)");

inline constexpr std::uint32_t kSegMagic = 0x47434632u; // "GCF2"
inline constexpr std::uint16_t kLayoutVer = 2;

enum class SegKind : std::uint16_t { Board = 1, BcastRing = 2, SpscQueue = 3 };

// 段头:64B,独占一 cache line。放在每个 v2 段的最前面。
struct alignas(64) SegHeader {
  std::uint32_t magic;          // = kSegMagic
  std::uint16_t layout_version; // = kLayoutVer
  SegKind kind;                 // Board / BcastRing / SpscQueue
  std::uint32_t entry_size;     // sizeof(Entry / Slot)
  std::uint32_t capacity;       // ring/queue: 2 的幂;board: N_GIDS
  std::uint64_t schema_hash;    // 字段布局的编译期 FNV(防错位;先告警)
  std::uint64_t created_ns;     // shm_init 建段时刻(诊断用,可为 0)
  std::uint8_t _rsvd[32];       // 同版本内加字段可填此处,sizeof 不变
};
static_assert(sizeof(SegHeader) == 64, "SegHeader must be exactly one cache line");
static_assert(alignof(SegHeader) == 64, "SegHeader must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<SegHeader>, "SegHeader must be POD (SHM contract)");
// 段头字段偏移是 attach 校验的契约:任一 bin padding 不同会让 seg_check 读错字段(#82)。
static_assert(offsetof(SegHeader, magic) == 0 && offsetof(SegHeader, entry_size) == 8 && offsetof(SegHeader, capacity) == 12
                  && offsetof(SegHeader, schema_hash) == 16,
              "SegHeader field offsets are part of the attach contract");

// 编译期 FNV-1a:对"字段布局描述串"求哈希,放进 schema_hash 防字段错位。
// 改了字段布局就改描述串 → 哈希变 → attach 时告警(提示双方版本不一致)。
constexpr std::uint64_t schema_fnv(const char *s) noexcept {
  std::uint64_t h = 1469598103934665603ull;
  for (; *s; ++s)
    h = (h ^ static_cast<std::uint8_t>(*s)) * 1099511628211ull;
  return h;
}

// shm_init 建段后调用:写好段头。
inline void seg_init(SegHeader &h, SegKind kind, std::uint32_t entry_size, std::uint32_t capacity, std::uint64_t schema_hash,
                     std::uint64_t now_ns) noexcept {
  h.magic = kSegMagic;
  h.layout_version = kLayoutVer;
  h.kind = kind;
  h.entry_size = entry_size;
  h.capacity = capacity;
  h.schema_hash = schema_hash;
  h.created_ns = now_ns;
}

// attach 校验结果。BadMagic/BadVersion/BadKind/AbiMismatch = 致命(布局根本对不上);
// SchemaDrift = 字段级演进期可容忍(调用方决定:通常告警后继续)。
enum class SegError : std::uint8_t { Ok, BadMagic, BadVersion, BadKind, AbiMismatch, SchemaDrift };

[[nodiscard]] constexpr std::string_view seg_error_str(SegError e) noexcept {
  switch (e) {
  case SegError::Ok:
    return "ok";
  case SegError::BadMagic:
    return "bad magic (wrong/uninit segment, or cross-endian peer)";
  case SegError::BadVersion:
    return "layout_version mismatch";
  case SegError::BadKind:
    return "wrong segment kind";
  case SegError::AbiMismatch:
    return "entry_size/capacity mismatch (rebuild shm_init?)";
  case SegError::SchemaDrift:
    return "schema_hash drift (field layout changed)";
  }
  return "?";
}

// attach 方:把段头与本进程编译期常量逐字段比对(纯函数,不打印,不分配)。
// 返回第一处不符;全符 → Ok。SchemaDrift 是最弱级(布局尺寸都对,只是描述串哈希不同)。
[[nodiscard]] inline SegError seg_check(const SegHeader &h, SegKind kind, std::uint32_t entry_size, std::uint32_t capacity,
                                        std::uint64_t schema_hash) noexcept {
  if (h.magic != kSegMagic)
    return SegError::BadMagic;
  if (h.layout_version != kLayoutVer)
    return SegError::BadVersion;
  if (h.kind != kind)
    return SegError::BadKind;
  if (h.entry_size != entry_size || h.capacity != capacity)
    return SegError::AbiMismatch;
  if (h.schema_hash != schema_hash)
    return SegError::SchemaDrift;
  return SegError::Ok;
}

} // namespace gconf::shm::v2
