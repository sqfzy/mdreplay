#pragma once
// sink.hpp — 输出编码器接口。按 [[output]].format 选具体实现(book_sink / trade_sink)。
// 输入里 Book 行喂 book sink、Trade 行喂 trade sink(main 按 Record.kind 路由)。

#include "error.hpp"
#include "record.hpp"

namespace mdreplay {

struct Sink {
  virtual ~Sink() = default;
  virtual Result<void> write(const Record&) = 0;
};

}  // namespace mdreplay
