// replay_feed.hpp — 把两个 RawTable(OKX+BN) 当作历史行情"重放"成事件流喂引擎
//
// 这是引擎的一种数据源(实盘路径的离线孪生): 按 ts 全局归并 OKX 与 BN 的行,
// OB 行→ObEvent, 成交行→TradeEvent, BN 的 OB 行→BnMidEvent(只取 mid), 末尾 finish。
// 未来 WsFeed/ShmFeed 产同样事件即可复用同一引擎。
#pragma once

#include <cstddef>

#include <spdlog/spdlog.h>

#include "event.hpp"
#include "streaming_engine.hpp"
#include "tick_feat.hpp"

namespace tick_feat::live {

// 从 RawTable 的第 i 行(OB 行)造 ObEvent。
inline ObEvent ob_event_at(const RawTable& t, std::size_t i) {
    return ObEvent{t.ts[i], t.bid_px0[i], t.ask_px0[i],
                   {t.bid_sz[0][i], t.bid_sz[1][i], t.bid_sz[2][i], t.bid_sz[3][i], t.bid_sz[4][i]},
                   {t.ask_sz[0][i], t.ask_sz[1][i], t.ask_sz[2][i], t.ask_sz[3][i], t.ask_sz[4][i]}};
}

// 全局按 ts 归并(同 ts 时 OKX 优先)喂引擎; OKX 的 OB/成交按 price==0 区分, BN 只取 OB 的 mid。
inline void replay(const RawTable& okx, const RawTable& bn, StreamingFeatureEngine& engine) {
    std::size_t i = 0, j = 0;
    std::size_t ob_cnt = 0, trade_cnt = 0, bn_cnt = 0;
    while (i < okx.rows() || j < bn.rows()) {
        const bool take_okx = (j >= bn.rows()) || (i < okx.rows() && okx.ts[i] <= bn.ts[j]);
        if (take_okx) {
            if (okx.price[i] == 0) { engine.feed_okx_ob(ob_event_at(okx, i)); ++ob_cnt; }
            else { engine.feed_okx_trade({okx.ts[i], okx.side[i], okx.price[i], okx.amount[i]}); ++trade_cnt; }
            ++i;
        } else {
            if (bn.price[j] == 0) { engine.feed_bn_mid({bn.ts[j], bn.bid_px0[j], bn.ask_px0[j]}); ++bn_cnt; }
            ++j;   // BN 成交行无用, 跳过
        }
    }
    engine.finish();
    spdlog::info("replay: okx_ob={} okx_trade={} bn_mid={} → {} 行特征",
                 ob_cnt, trade_cnt, bn_cnt, engine.features().rows());
}

} // namespace tick_feat::live
