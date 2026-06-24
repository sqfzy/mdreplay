// main.cpp — mdreplay CLI 入口。
//
// 阶段 1 骨架:仅验证 gconf v2 契约(trade.h/board.h 的 static_assert)、核心类型、include 路径、
// C++23 std::expected 与 spdlog 均可编译/链接。编排逻辑(load→discover→replay)在后续阶段填入。

#include <cstdio>

#include <gconf/shm/v2/board.h>    // BBO Board(含 symbols.h)
#include <gconf/shm/v2/names.h>    // 段名(BOOK/TRADE/...)
#include <gconf/shm/v2/trade.h>    // 成交环(TradePayload/TradeEntry/TradeRing)

#include "error.hpp"
#include "record.hpp"

int main() {
  namespace v2 = gconf::shm::v2;
  static_assert(sizeof(v2::TradeEntry) == 64 && sizeof(v2::BoardSlot) == 64);

  std::printf("mdreplay skeleton ok | BOOK=%s | TRADE=%s | symbols=%d\n",
              v2::BOOK, v2::TRADE, static_cast<int>(gconf::sym::N_GLOBAL_SYMBOL_IDS));

  // 占位:让核心类型参与编译,避免未使用告警。
  const mdreplay::Record probe{};
  return probe.kind == mdreplay::Kind::Book ? 0 : 0;
}
