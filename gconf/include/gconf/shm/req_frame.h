#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gconf/domain/enums.h>   // ExType / AcType / OrderMode

// Strategy → trade-engine request FRAME — venue-neutral, VERSION-neutral.
//
// PrivateReqFrame: 64B, `type` discriminates the union; prices/qty are UNIFIED
// internal precision × 1e8. Shared by BOTH SHM layouts — v1 carries it in
// gconf::shm::v1::PrivateReqRing (shm/v1/req.h), v2 in gconf::shm::v2::ReqQueue
// (shm/v2/req.h). The frame format is identical; only the queue mechanism differs.

namespace gconf::priv {

// target_path 哨兵: 不指定路 → 下单进程 pick_best_path 选最快路 (策略真单默认)。
inline constexpr std::uint8_t REQ_TARGET_PATH_AUTO = 0xFF;

enum class PrivateOrderReqType : std::uint8_t {
  WS_REQUEST_PLACE_ORDER = 0,
  WS_REQUEST_CANCEL_ORDER = 1,
  WS_REQUEST_MODIFY_ORDER = 2,
  WS_REQUEST_QUERY_ORDER = 3,
};

struct alignas(8) PlaceOrderReq {
  std::uint64_t client_order_id{};                          // 8B 自生成订单 id
  std::uint64_t price_1e8 = 0;                              // 8B limit 用, market 可 0; × 1e8
  std::uint64_t qty_1e8 = 0;                                // 8B × 1e8
  std::uint16_t local_symbol_id = 0;                         // 2B subset27 local symbol id (见 local_symbol_id_name; 非 global symbol id)
  enums::OrderMode order_mode = enums::OrderMode::BUY_OPEN_LIMIT; // 1B
  std::uint8_t pad_[5]{};                                   // 5B
                                                            // Total 32B
};
static_assert(sizeof(PlaceOrderReq) == 32, "PlaceOrderReq must be 32B");
static_assert(offsetof(PlaceOrderReq, price_1e8) == 8 && offsetof(PlaceOrderReq, qty_1e8) == 16 &&
              offsetof(PlaceOrderReq, local_symbol_id) == 24,
              "PlaceOrderReq money/id field offsets are the SHM contract (#88)");

struct alignas(8) ModifyOrderReq {                          // 与 Place 字段一致；类型独立避免误用
  std::uint64_t client_order_id{};                          // 8B
  std::uint64_t price_1e8 = 0;                              // 8B
  std::uint64_t qty_1e8 = 0;                                // 8B
  std::uint16_t local_symbol_id = 0;                         // 2B subset27 local symbol id (见 local_symbol_id_name; 非 global symbol id)
  enums::OrderMode order_mode = enums::OrderMode::BUY_OPEN_LIMIT; // 1B
  std::uint8_t pad_[5]{};                                   // 5B
                                                            // Total 32B
};
static_assert(sizeof(ModifyOrderReq) == 32, "ModifyOrderReq must be 32B");

struct alignas(8) QueryOrderReq {   // 用于查单 / 撤单两个方法
  std::uint64_t client_order_id{};  // 8B
  std::uint16_t local_symbol_id = 0; // 2B subset27 local symbol id (见 local_symbol_id_name; 非 global symbol id)
  std::uint8_t pad_[6]{};           // 6B
                                    // Total 16B
};
static_assert(sizeof(QueryOrderReq) == 16, "QueryOrderReq must be 16B");

// 策略 → 交易引擎 SHM 请求帧
struct alignas(64) PrivateReqFrame {
  PrivateOrderReqType type;    // 1B 判别字段
  std::uint8_t account_idx;    // 1B 账户编号
  enums::ExType ex;            // 1B 交易所(消费端须按 ExType::Count 校验范围:旧版/异常 producer 可能越界)
  enums::AcType ac;            // 1B 账户/产品类型(同上,按 AcType::Count 校验)
  std::uint8_t target_path;    // 1B 指定下单路号; REQ_TARGET_PATH_AUTO = pick_best
  std::uint8_t _pad[3];        // 3B 对齐
  std::uint64_t submit_ns;  // 8B 策略侧提交时刻 (epoch ns)
  union U {
    PlaceOrderReq place;   // type=WS_REQUEST_PLACE_ORDER
    ModifyOrderReq modify; // type=WS_REQUEST_MODIFY_ORDER
    QueryOrderReq query;   // type=WS_REQUEST_QUERY_ORDER / CANCEL_ORDER
  } u;
  std::uint8_t _tail_pad[16]{}; // 16B 尾部填充到 64B
                                // Total 64B
};
static_assert(sizeof(PrivateReqFrame) == 64, "PrivateReqFrame must be 64B");
static_assert(alignof(PrivateReqFrame) == 64, "PrivateReqFrame must be 64B aligned");
static_assert(std::is_trivially_copyable_v<PrivateReqFrame>);
// union 偏移靠 _pad[3] 补到 8 对齐;增删头部字段会悄悄移动 union 而 sizeof 仍 64B →
// 跨进程读 place/modify/query 整体错位(price/qty/client_order_id 错乱)。锁死 union 与 ts 偏移(#88)。
static_assert(offsetof(PrivateReqFrame, submit_ns) == 8 && offsetof(PrivateReqFrame, u) == 16,
              "PrivateReqFrame union offset is the SHM contract — _pad keeps u@16");

} // namespace gconf::priv
