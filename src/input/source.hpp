#pragma once
// source.hpp — 单个输入源的有序游标接口 + 内存载入实现。merge 通过 peek/advance 做 N 路归并。
//
// 契约:同一 Source 产出的 Record 必须按 ts_ns 非降序(单文件天然有序);peek() 返回当前头,
// nullptr = 耗尽。各格式解析器(csv/json)都把记录载入 LoadedSource —— 核心只认本接口,与格式无关。

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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

// 已载入内存的有序 Record 源(csv/json 解析器的共同产物)。
class LoadedSource : public Source {
public:
  explicit LoadedSource(std::vector<Record> rows) : rows_(std::move(rows)) {}
  [[nodiscard]] const Record* peek() override { return i_ < rows_.size() ? &rows_[i_] : nullptr; }
  void                        advance() override { ++i_; }

private:
  std::vector<Record> rows_;
  std::size_t         i_ = 0;
};

}  // namespace mdreplay
