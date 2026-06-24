#pragma once
// report.hpp — 回放观测:累计 book/trade/skipped,按墙钟每 progress_sec 打一次进度,收尾汇总。

#include <chrono>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "core/record.hpp"

namespace mdreplay {

class Reporter {
public:
  explicit Reporter(int progress_sec) : interval_(progress_sec), last_(clock::now()) {}

  void on_event(const Record& r) {
    if (r.kind == Kind::Book) ++book_; else ++trade_;
    last_ts_ = r.ts_ns;
    if (interval_ > 0) {
      const auto now = clock::now();
      if (now - last_ >= std::chrono::seconds(interval_)) {
        spdlog::info("replay: book={} trade={} skipped={} ts={}", book_, trade_, skipped_, last_ts_);
        last_ = now;
      }
    }
  }

  void add_skipped(std::uint64_t n) { skipped_ += n; }

  void finish() const {
    spdlog::info("done: {} events (book={} trade={}), skipped={}", book_ + trade_, book_, trade_,
                 skipped_);
  }

private:
  using clock = std::chrono::steady_clock;

  int                     interval_;
  clock::time_point       last_;
  std::uint64_t           book_{0}, trade_{0}, skipped_{0};
  std::int64_t            last_ts_{0};
};

}  // namespace mdreplay
