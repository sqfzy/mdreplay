#pragma once
// source.hpp — 单个输入源的有序游标接口。merge 通过 peek/advance 做 N 路归并。
//
// 契约:同一 Source 产出的 Record 必须按 ts_ns 非降序(单文件天然有序);peek() 返回当前头,
// nullptr = 耗尽。实现:csv_source(本期)/ json_source(以后)—— 核心只认本接口,与格式无关。

#include "record.hpp"

namespace mdreplay {

struct Source {
  virtual ~Source() = default;
  [[nodiscard]] virtual const Record* peek() = 0;  // 当前头;nullptr = 耗尽
  virtual void advance() = 0;                       // 推进到下一条
};

}  // namespace mdreplay
