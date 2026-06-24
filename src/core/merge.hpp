#pragma once
// merge.hpp — N 路归并:把多个各自有序的 Source 合成一条全局有序 Record 流。
//
// 序 = (ts_ns, 源序)稳定确定 —— 这是回放可复现的命根,通用 tiebreak,不含任何 venue/口径规则。
// 时间窗 [start_ns, end_ns] 闭区间过滤:头 < start 跳过;全局最小 > end 即提前终止(各源已有序)。

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <vector>

#include "core/record.hpp"
#include "core/window.hpp"  // kNoStart / kNoEnd
#include "input/source.hpp"

namespace mdreplay {

class Merger {
public:
  Merger(std::vector<std::unique_ptr<Source>> sources, std::int64_t start_ns, std::int64_t end_ns)
      : sources_(std::move(sources)), start_ns_(start_ns), end_ns_(end_ns) {
    for (std::size_t i = 0; i < sources_.size(); ++i) push_head(i);
  }

  // 取下一条(全局 (ts,源序) 序、窗口内);耗尽或越过窗口上界 → nullopt。
  [[nodiscard]] std::optional<Record> next() {
    while (!heap_.empty()) {
      const HeapItem top = heap_.top();
      heap_.pop();
      const Record rec = *sources_[top.src]->peek();  // 入堆时已确保非空
      sources_[top.src]->advance();
      push_head(top.src);

      if (rec.ts_ns > end_ns_) return std::nullopt;    // 全局最小都超上界 → 后面更大,终止
      if (rec.ts_ns < start_ns_) continue;             // 早于窗口 → 跳过,不发
      return rec;
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

  std::vector<std::unique_ptr<Source>>                                    sources_;
  std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<>>     heap_;
  std::int64_t                                                            start_ns_;
  std::int64_t                                                            end_ns_;
};

}  // namespace mdreplay
