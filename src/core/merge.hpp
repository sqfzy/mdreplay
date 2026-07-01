#pragma once
// merge.hpp — N 路归并:把多个各自有序的 Source 合成一条全局有序 Record 流,并标记每条来自哪个 unit。
//
// 序 = (ts_ns, 源序)稳定确定 —— 这是回放可复现的命根,通用 tiebreak,不含任何 venue/口径规则。
// 多路回放:每个 Source 归属一个 unit(= 一个 [[replays]]),next() 返回 {rec, unit} 供按 unit 路由到
// 各自的 Sink。时间窗 [start,end] 闭区间**按 unit** 过滤(每路独立窗):某源头 < 本路 start 跳过、
// > 本路 end 则该源出堆(其余更大,无需再看)。全局终止 = 堆空。
//
// 源序与 unit:main 把各 unit 的 sources 按 unit-major 拼成一个全局列表,故源下标天然编码 (unit, 源内序);
// tiebreak (ts, 源下标) 即 (ts, unit, 源内序),确定且与单 unit 时逐字节一致(N=1 退化为旧行为)。

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "core/record.hpp"
#include "input/source.hpp"

namespace mdreplay {

// 归并产出:一条 Record + 它所属的 unit(= 路由到 outputs[unit])。
struct Tagged {
  Record      rec;
  std::size_t unit;
};

class Merger {
public:
  // sources:全局源列表(unit-major 拼接);src_unit[i]=源 i 的 unit;unit_window[u]={start,end} 本路窗口。
  Merger(std::vector<std::unique_ptr<Source>> sources, std::vector<std::size_t> src_unit,
         std::vector<std::pair<std::int64_t, std::int64_t>> unit_window)
      : sources_(std::move(sources)),
        src_unit_(std::move(src_unit)),
        unit_window_(std::move(unit_window)) {
    for (std::size_t i = 0; i < sources_.size(); ++i) push_head(i);
  }

  // 取下一条(全局 (ts,源序) 序、各 unit 窗口内),附带 unit;耗尽 → nullopt。
  [[nodiscard]] std::optional<Tagged> next() {
    while (!heap_.empty()) {
      const HeapItem top = heap_.top();
      heap_.pop();
      const std::size_t  unit = src_unit_[top.src];
      const auto [start, end] = unit_window_[unit];
      const Record rec = *sources_[top.src]->peek();  // 入堆时已确保非空
      sources_[top.src]->advance();

      if (rec.ts_ns > end) continue;     // 超本路上界 → 该源后续更大,**不再 push_head**(出堆)
      push_head(top.src);                // 仍在窗内/早于窗 → 继续供给该源下一条
      if (rec.ts_ns < start) continue;   // 早于本路下界 → 跳过,不发
      return Tagged{rec, unit};
    }
    return std::nullopt;
  }

private:
  // 堆元素:按 (ts, 源序) 排序;源序作 tiebreak 保证稳定确定。
  struct HeapItem {
    std::int64_t ts;
    std::size_t  src;
    bool operator>(const HeapItem& o) const noexcept {
      return ts != o.ts ? ts > o.ts : src > o.src;     // 小顶堆:ts 小优先,同 ts 源序小优先
    }
  };

  void push_head(std::size_t i) {
    if (const Record* r = sources_[i]->peek()) heap_.push(HeapItem{r->ts_ns, i});
  }

  std::vector<std::unique_ptr<Source>>                                sources_;
  std::vector<std::size_t>                                            src_unit_;
  std::vector<std::pair<std::int64_t, std::int64_t>>                  unit_window_;
  std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<>> heap_;
};

}  // namespace mdreplay
