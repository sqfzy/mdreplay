#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gconf/shm/req_frame.h>   // gconf::priv::PrivateReqFrame (the shared frame)

// v1 策略 → 交易引擎请求环(SPMC 广播)。帧格式见 common/req_frame.h(版本中立)。
// v2 的对应物是 gconf::shm::v2::ReqQueue(per-venue SPSC + 背压,shm/v2/req.h)。

namespace gconf::shm::v1 {

// 私有请求 ring 默认容量: 策略下单 TPS 通常 < 200, 峰值 < 1000; 1024 留足 backlog.
inline constexpr std::size_t PRIVATE_REQ_RING_CAP = 1024;

// SPMC broadcast ring: 1 producer (策略), N consumers (各 TE 进程按 frame.ex/ac 过滤)。
// SHM 里只有 head; entries 只读不回收; 各 consumer 进度互不影响。
struct PrivateReqRing {
  static constexpr std::size_t CAP = PRIVATE_REQ_RING_CAP;
  static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of 2");
  static constexpr std::size_t MASK = CAP - 1;

  using Entry = gconf::priv::PrivateReqFrame;

  alignas(64) std::atomic<std::uint64_t> head{0};  // producer 写位置 (slot & MASK = 下标)
  alignas(64) Entry entries[CAP];
};

} // namespace gconf::shm::v1
