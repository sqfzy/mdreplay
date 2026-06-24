#pragma once
#include <atomic>
#include <cstdint>

#include <gconf/shm/v1/private_event.h>   // PrivateEventRing (reused as the order-ACK ring)

// Order-entry ACK event SHM contract — bn-standard.
//
// Distinct from PrivateEventEntry (fills): the ACK carries err_code + resp_type
// and is mostly reserved padding. Carried in the SAME PrivateEventRing template,
// instantiated over OrderEventEntry.

namespace gconf::shm::v1 {

struct alignas(64) OrderEventEntry {
  std::atomic<std::uint64_t> seq{0}; // 8B 编码 = (slot << 1) | writing_bit; 0 = 未写过
  std::uint64_t client_order_id_digits;          // 8B clientOrderId 累加成 uint64
  std::uint64_t T_ns;                // 8B transaction time
  std::uint16_t local_symbol_id;          // 2B subset27 local id (local symbol id)
  std::uint16_t err_code;            // 2B 错误码, 0 = 无错误  (⚠ Bybit retCode 可能 > u16, 见 bybit/TODO.md)
  std::uint8_t status;               // 1B enums::OrderStatus
  std::uint8_t account_id;           // 1B 账户编号
  std::uint8_t resp_type;            // 1B enums::PrivateOrderRespType
  std::uint8_t _pad[31]{};           // 31B reserved (补齐 64B)
                                     // Total 64B

  void write_begin(std::uint64_t slot) noexcept { seq.store((slot << 1) | 1ULL, std::memory_order_release); } // 奇, 写中
  void write_end(std::uint64_t slot) noexcept { seq.store(slot << 1, std::memory_order_release); }            // 偶, 可读
};
static_assert(sizeof(OrderEventEntry) == 64, "OrderEventEntry must be 64B (1 cache line)");
static_assert(alignof(OrderEventEntry) == 64, "OrderEventEntry must be 64B aligned");

} // namespace gconf::shm::v1
