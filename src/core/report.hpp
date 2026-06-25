#pragma once
// report.hpp — 回放观测:累计 book/trade/skipped,按墙钟每 progress_sec 打一次进度,收尾汇总。

#include <chrono>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "core/record.hpp"
#include "core/skip.hpp"

namespace mdreplay {

// 流式下 skip 在消费期惰性累加,故 Reporter 持 SkipStats 引用、周期/收尾**实时读** total()
// (而非起始 seed)——回放中能看 skip 实时增长,收尾即最终值。SkipStats 须活过回放。
class Reporter {
public:
  Reporter(int progress_sec, const SkipStats& skips)
      : interval_(progress_sec), skips_(&skips), last_(clock::now()) {}

  void on_event(const Record& r) {
    if (r.kind == Kind::Book) ++book_; else ++trade_;
    last_ts_ = r.ts_ns;
    if (interval_ > 0) {
      const auto now = clock::now();
      if (now - last_ >= std::chrono::seconds(interval_)) {
        spdlog::info("replay: book={} trade={} skipped={} ts={}", book_, trade_, skips_->total(),
                     last_ts_);
        last_ = now;
      }
    }
  }

  void finish() const {
    spdlog::info("done: {} events (book={} trade={}), skipped={}", book_ + trade_, book_, trade_,
                 skips_->total());
  }

private:
  using clock = std::chrono::steady_clock;

  int                     interval_;
  const SkipStats*        skips_;
  clock::time_point       last_;
  std::uint64_t           book_{0}, trade_{0};
  std::int64_t            last_ts_{0};
};

}  // namespace mdreplay
