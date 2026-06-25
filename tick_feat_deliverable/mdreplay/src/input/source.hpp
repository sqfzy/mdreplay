#pragma once
// source.hpp — 单个输入源的有序游标接口。merge 通过 peek/advance 做 N 路归并,与格式无关。
//
// 契约:同一 Source 产出的 Record 必须按 ts_ns 非降序(单文件天然有序);peek() 返回当前头,
// nullptr = 耗尽。各格式的**流式**实现(csv.hpp::StreamingCsvSource / json.hpp::StreamingJsonSource)
// 在 advance() 时才惰性解析下一行——核心只认本接口,只缓冲 1 条,峰值内存 O(文件数)。

#include <string>

#include "core/record.hpp"

namespace mdreplay {

// 去掉行尾 '\r'(健壮吃 CRLF,如 Python csv.writer 的 \r\n)。csv/json 解析共用。
inline void chomp_cr(std::string& s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
}

struct Source {
  virtual ~Source() = default;
  [[nodiscard]] virtual const Record* peek() = 0;  // 当前头;nullptr = 耗尽
  virtual void advance() = 0;                       // 推进到下一条
};

}  // namespace mdreplay
