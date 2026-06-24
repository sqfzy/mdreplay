// streaming_engine.hpp — compute_day 的「事件流增量执行」形式
//
// 把离线批处理改写成: 事件到达 → 当前秒桶累积 → 跨秒结算一行 f0-f9。
// 滚动窗口用 cumsum 增量 append(非 deque running-sum), 与离线 np.cumsum 同序;
// 桶内成交用 Kahan(同 pandas); as-of 复用 prev_index —— 三者保证与离线逐位一致。
//
// ⚠️ 三个口径在流式下的落地:
//  1. 只对「有 OB 的秒」产行(同离线 grid=bucket_last_mid 的秒); 纯成交秒(无 OB)整秒丢弃、其成交也丢
//     (同离线 grid_mid.merge(grp, how='left') 丢掉 trade-only 秒)。
//  2. mid=桶末 OB; imb/spread=ts≤秒起点的最后 OB(boundary_ob, 不同快照):
//     进入新秒时 boundary=latest_ob(ts<新秒); 桶内遇 ts==秒起点 的 OB 再刷新(匹配离线 prev_index side=right)。
//  3. 多源 as-of: 跨秒先结算 BN 桶(append bn 桶末 mid)再 OKX 桶, 保证 okx 秒 s 的 pdiff 用到 bn 秒 s。
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "event.hpp"
#include "tick_feat.hpp"

namespace tick_feat::live {

class StreamingFeatureEngine {
public:
    StreamingFeatureEngine() = default;

    void feed_okx_ob(const ObEvent& ev) {
        advance_to(ev.ts);
        latest_ob_ = ev; have_latest_ob_ = true;
        bucket_last_mid_ = ob_mid_scaled(ev.bid_px0, ev.ask_px0);
        has_ob_this_sec_ = true;
        if (ev.ts == cur_sec_) { boundary_ob_ = ev; have_boundary_ob_ = true; }  // ts≤秒起点 含 ts==秒起点
    }

    void feed_okx_trade(const TradeEvent& ev) {
        advance_to(ev.ts);
        const double usd  = ev.amount * static_cast<double>(ev.price_scaled) / PX_SCALE;
        const double sign = (ev.side == 0) ? 1.0 : -1.0;
        kahan_add(sv_, sv_comp_, sign * usd);
        kahan_add(av_, av_comp_, usd);
    }

    void feed_bn_mid(const BnMidEvent& ev) {
        advance_to(ev.ts);
        bn_bucket_last_mid_  = ob_mid_scaled(ev.bid_px0, ev.ask_px0);
        bn_has_mid_this_sec_ = true;
    }

    void finish() {                                  // 结算最后未闭合的秒
        if (started_) { settle_bn_sec(); settle_okx_sec(); }
    }

    const Features& features() const { return out_; }

private:
    // watermark 推进: 跨秒时先结算 cur_sec(先 bn 后 okx), 再开启新秒。
    void advance_to(int64_t ts) {
        const int64_t new_sec = to_bucket(ts);
        if (new_sec == cur_sec_) return;             // 同秒, 继续累积
        if (started_) { settle_bn_sec(); settle_okx_sec(); }
        cur_sec_ = new_sec; started_ = true;
        has_ob_this_sec_ = false;
        sv_ = sv_comp_ = av_ = av_comp_ = 0.0;
        boundary_ob_ = latest_ob_; have_boundary_ob_ = have_latest_ob_;  // 秒首 as-of=进入新秒时最新盘口
        bn_has_mid_this_sec_ = false;
    }

    void settle_bn_sec() {
        if (!bn_has_mid_this_sec_) return;
        bn_ts_.push_back(cur_sec_);
        bn_mid_.push_back(bn_bucket_last_mid_);
    }

    void settle_okx_sec() {
        if (!has_ob_this_sec_) return;               // 只对有 OB 的秒产行
        const int64_t q   = cur_sec_;
        const double  mid = bucket_last_mid_;         // 桶末 mid

        // imb/spread: 秒首 as-of(boundary_ob); 无则 0(同离线 prev_index<0)
        double imb1 = 0.0, imb5 = 0.0, spread = 0.0;
        if (have_boundary_ob_) {
            double bsz5 = 0.0, asz5 = 0.0;
            for (int l = 0; l < 5; ++l) {            // 逐项 /PX_SCALE 再累加(同离线 extract_ob_snapshots)
                bsz5 += boundary_ob_.bid_sz[l] / PX_SCALE;
                asz5 += boundary_ob_.ask_sz[l] / PX_SCALE;
            }
            const ImbSpread is = imb_spread_from_levels(
                static_cast<double>(boundary_ob_.bid_px0) / PX_SCALE,
                static_cast<double>(boundary_ob_.ask_px0) / PX_SCALE,
                boundary_ob_.bid_sz[0] / PX_SCALE, boundary_ob_.ask_sz[0] / PX_SCALE,
                bsz5, asz5);
            imb1 = is.imb1; imb5 = is.imb5; spread = is.spread;
        }

        // log return(1s 网格): log(mid/prev_mid); 首秒/prev≤0 → 0
        const double lr = (have_prev_mid_ && prev_mid_ > 0.0) ? std::log(mid / prev_mid_) : 0.0;

        // append 网格 + cumsum 增量(顺序累加, 同 np.cumsum)
        ts_sec_.push_back(q); mid_.push_back(mid);
        imb1_.push_back(imb1); imb5_.push_back(imb5); spread_.push_back(spread);
        cs_svol_.push_back((cs_svol_.empty() ? 0.0 : cs_svol_.back()) + sv_);
        cs_avol_.push_back((cs_avol_.empty() ? 0.0 : cs_avol_.back()) + av_);
        cs_lr2_.push_back((cs_lr2_.empty() ? 0.0 : cs_lr2_.back()) + lr * lr);

        // pdiff: bn 桶末 mid as-of(ts≤q 的最后 bn 秒)
        double pdiff = 0.0, pcnt = 0.0;
        const std::int64_t bi = prev_index(bn_ts_, q);
        if (bi >= 0) {
            const double bn_m = bn_mid_[static_cast<std::size_t>(bi)];
            if (bn_m > 0.0 && std::isfinite(mid) && mid > 0.0) {
                pdiff = (mid - bn_m) / bn_m * 1e4; pcnt = 1.0;
            }
        }
        cs_pdiff_.push_back((cs_pdiff_.empty() ? 0.0 : cs_pdiff_.back()) + pdiff);
        cs_pcnt_.push_back((cs_pcnt_.empty() ? 0.0 : cs_pcnt_.back()) + pcnt);

        prev_mid_ = mid; have_prev_mid_ = true;

        emit_features(q, mid, imb1, imb5, spread);
    }

    // 逐秒 f0-f9(与 compute_day 逐秒部分逐字一致: 同 rolling_sum_asof / grid_value_asof)
    void emit_features(int64_t q, double mid_now, double imb1, double imb5, double spread) {
        const double svol60  = rolling_sum_asof(cs_svol_, ts_sec_, q, 60);
        const double avol60  = rolling_sum_asof(cs_avol_, ts_sec_, q, 60);
        const double svol10  = rolling_sum_asof(cs_svol_, ts_sec_, q, 10);
        const double avol10  = rolling_sum_asof(cs_avol_, ts_sec_, q, 10);
        const double lr2_300 = rolling_sum_asof(cs_lr2_, ts_sec_, q, 300);
        const double mid_60  = grid_value_asof(ts_sec_, mid_, q - 60  * GRID_US);
        const double mid_300 = grid_value_asof(ts_sec_, mid_, q - 300 * GRID_US);

        const double svol_ratio = (avol60 > 1e-9) ? svol60 / avol60 : 0.0;
        const double vpin       = (avol10 > 1e-9) ? std::abs(svol10) / avol10 : -1.0;
        const double trend60    = (mid_now > 0) ? (mid_now - mid_60)  / mid_now * 1e4 : 0.0;
        const double trend300   = (mid_now > 0) ? (mid_now - mid_300) / mid_now * 1e4 : 0.0;
        const double rv_300s    = std::sqrt(std::max(lr2_300, 0.0)) * 1e4;

        const double pm_2h_s  = rolling_sum_asof(cs_pdiff_, ts_sec_, q, 7200);
        const double pm_2h_n  = rolling_sum_asof(cs_pcnt_,  ts_sec_, q, 7200);
        const double pm_2h    = (pm_2h_n  > 0) ? pm_2h_s  / pm_2h_n  : 0.0;
        const double pm_12h_s = rolling_sum_asof(cs_pdiff_, ts_sec_, q, 43200);
        const double pm_12h_n = rolling_sum_asof(cs_pcnt_,  ts_sec_, q, 43200);
        const double pm_12h   = (pm_12h_n > 0) ? pm_12h_s / pm_12h_n : 0.0;

        out_.ts_us.push_back(q);
        out_.f0.push_back(imb1);  out_.f1.push_back(imb5);  out_.f2.push_back(spread);
        out_.f3.push_back(svol_ratio); out_.f4.push_back(vpin);
        out_.f5.push_back(trend60); out_.f6.push_back(trend300); out_.f7.push_back(rv_300s);
        out_.f8.push_back(pm_2h); out_.f9.push_back(pm_12h);
        out_.mid_price.push_back(mid_now);
    }

    // ── OKX 当前秒桶 ──
    int64_t cur_sec_         = std::numeric_limits<int64_t>::min();
    bool    started_         = false;
    bool    has_ob_this_sec_ = false;
    double  bucket_last_mid_ = 0.0;
    double  sv_ = 0.0, sv_comp_ = 0.0, av_ = 0.0, av_comp_ = 0.0;
    bool    have_latest_ob_   = false; ObEvent latest_ob_{};
    bool    have_boundary_ob_ = false; ObEvent boundary_ob_{};

    // ── OKX 历史(append-only) ──
    std::vector<int64_t> ts_sec_;
    std::vector<double>  mid_, imb1_, imb5_, spread_;
    std::vector<double>  cs_svol_, cs_avol_, cs_lr2_;
    double prev_mid_      = 0.0;
    bool   have_prev_mid_ = false;

    // ── BN 当前桶 + 历史 ──
    bool    bn_has_mid_this_sec_ = false;
    double  bn_bucket_last_mid_  = 0.0;
    std::vector<int64_t> bn_ts_;
    std::vector<double>  bn_mid_;

    // ── pdiff 历史(与 okx 网格对齐) ──
    std::vector<double> cs_pdiff_, cs_pcnt_;

    Features out_;
};

} // namespace tick_feat::live
