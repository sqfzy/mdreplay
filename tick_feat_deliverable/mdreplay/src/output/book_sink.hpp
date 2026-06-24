#pragma once
// book_sink.hpp — Book Record → gconf v2 Board.slot[gid].write(BBO)。
//
// 单写者直接写(无需 claim_if_newer);update_id 取 ts_ns(单调,供消费者感知跳变);
// price_scale/qty_scale 沿用数据精度(已在 csv_source 推得);延迟字段回放置 0。
// 双所同 gid 在此 latest-wins(venue-agnostic),符合「不指定交易所」口径。

#include <cstdint>

#include <gconf/shm/v2/board.h>

#include "error.hpp"
#include "output/sink.hpp"
#include "record.hpp"

namespace mdreplay {

class BookSink : public Sink {
public:
  explicit BookSink(gconf::shm::v2::Board* board) : board_(board) {}

  Result<void> write(const Record& r) override {
    board_->slot[r.gid].write(r.ts_ns, static_cast<std::uint64_t>(r.ts_ns),  // update_id = ts(单调)
                              r.bid_px, r.bid_qty, r.ask_px, r.ask_qty,
                              0, 0, 0,  // kernel/recv/write 延迟:回放无
                              r.gid, r.price_scale, r.qty_scale, 0 /*path_idx*/);
    return {};
  }

private:
  gconf::shm::v2::Board* board_;
};

}  // namespace mdreplay
