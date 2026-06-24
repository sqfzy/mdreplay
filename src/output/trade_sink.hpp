#pragma once
// trade_sink.hpp — Trade Record → gconf v2 TradeRing.publish(无损广播,fetch_add + seqlock)。

#include <gconf/shm/v2/trade.h>

#include "error.hpp"
#include "output/sink.hpp"
#include "record.hpp"

namespace mdreplay {

class TradeSink : public Sink {
public:
  explicit TradeSink(gconf::shm::v2::TradeRing* ring) : ring_(ring) {}

  Result<void> write(const Record& r) override {
    gconf::shm::v2::TradePayload p;
    p.exch_ns          = r.ts_ns;
    p.px               = r.px;
    p.qty              = r.qty;
    p.global_symbol_id = r.gid;
    p.side             = r.side;
    p.price_scale      = r.price_scale;
    p.qty_scale        = r.qty_scale;
    ring_->publish(p);
    return {};
  }

private:
  gconf::shm::v2::TradeRing* ring_;
};

}  // namespace mdreplay
