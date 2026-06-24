// tick_feat.hpp — build_tick_feat_standalone.py 的 C++ 逐位复刻(算法核心, 纯逻辑)
//
// 目标: 与离线黄金参照 compute_day 的输出 max|diff| ≈ 机器精度。
// 数值流程严格照搬 python: 同样的 /1e8、同序 cumsum、同样的 as-of(prev_index) 口径。
// 本头文件不碰 IO(parquet 读写见 parquet_io.hpp), 便于纯逻辑单元测试。
//
// ⚠️ 必须复刻的口径(与 python docstring 一致, 否则 diff≠0):
//  - 1s 桶: ts_sec = ts / GRID_US * GRID_US
//  - mid(trend/rv/pdiff 用)= 桶内最后一个 OB; imb/spread = ts≤桶起点 的最后 OB(上一秒末, 不同快照!)
//  - 滚动和窗口 (q-window, q]: end=cs[prev(q)], begin=cs[prev(q-window*1e6-1)]
//  - f8/f9 = pdiff 滚动均值 pm = Σpdiff/Σ有效计数(去均值在 C++ 推理时做)
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace tick_feat {

inline constexpr int     N_LVLS   = 15;
inline constexpr int64_t GRID_US  = 1'000'000;        // 1s 桶
inline constexpr double  PX_SCALE = 1e8;              // 价/量统一 ×1e8 存储
inline constexpr int64_t DAY_US   = 86'400LL * GRID_US;

// 标准 64 列 parquet 布局(0-indexed, N_LVLS=15)
inline constexpr int COL_TS = 0, COL_SIDE = 1, COL_PRICE = 2, COL_AMOUNT = 3;
inline constexpr int COL_BID_PX = 4;                 // 买价 L0..14
inline constexpr int COL_BID_SZ = 4 + N_LVLS;        // 19  买量 L0..14
inline constexpr int COL_ASK_PX = 4 + 2 * N_LVLS;    // 34  卖价 L0..14
inline constexpr int COL_ASK_SZ = 4 + 3 * N_LVLS;    // 49  卖量 L0..14

// ── 输入: 一个 (symbol,date) 标准 parquet 的列式原始数据 ──────────────────
//   行按 ts 升序; OB 行 price==0, 成交行 price>0(价均 ×1e8 的 int64)。
struct RawTable {
    std::vector<int64_t>               ts;       // col0 微秒
    std::vector<int64_t>               side;     // col1 0买/1卖(仅成交行)
    std::vector<int64_t>               price;    // col2 ×1e8, OB行=0
    std::vector<double>                amount;   // col3 成交量(币)
    std::vector<int64_t>               bid_px0;  // col4
    std::vector<int64_t>               ask_px0;  // col34
    std::array<std::vector<double>, 5> bid_sz;   // col19..23 前5档买量
    std::array<std::vector<double>, 5> ask_sz;   // col49..53 前5档卖量

    std::size_t rows() const { return ts.size(); }
};

struct Features {
    std::vector<int64_t> ts_us;
    std::vector<double>  f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, mid_price;

    std::size_t rows() const { return ts_us.size(); }
};

// ─────────────────────────────────────────────────────────────────────────
//  批量查询工具(对齐 numpy searchsorted/cumsum 语义)
// ─────────────────────────────────────────────────────────────────────────

// _prev_idx: np.searchsorted(sorted, q, side="right") - 1; q<sorted[0] 返回 -1。
inline std::int64_t prev_index(const std::vector<int64_t>& sorted, int64_t query) {
    auto it = std::upper_bound(sorted.begin(), sorted.end(), query);
    return static_cast<std::int64_t>(it - sorted.begin()) - 1;
}

// _grid_val: 取 ts≤query 的最后一个网格值(as-of); 无效返回 nan。
inline double grid_value_asof(const std::vector<int64_t>& ts_grid,
                              const std::vector<double>& value_grid, int64_t query) {
    const std::int64_t index = prev_index(ts_grid, query);
    if (index < 0) return std::nan("");
    return value_grid[static_cast<std::size_t>(index)];
}

// _rolling_sum: cumsum 差分得窗口 (query-window, query] 的和。
inline double rolling_sum_asof(const std::vector<double>& cumsum,
                               const std::vector<int64_t>& ts_grid,
                               int64_t query, int window_s) {
    const std::int64_t index_end   = prev_index(ts_grid, query);
    const std::int64_t index_begin = prev_index(ts_grid, query - int64_t(window_s) * GRID_US - 1);
    if (index_end < 0) return 0.0;
    const std::int64_t last = static_cast<std::int64_t>(cumsum.size()) - 1;
    const double end_value   = cumsum[static_cast<std::size_t>(std::clamp(index_end, int64_t{0}, last))];
    const double begin_value = (index_begin >= 0)
        ? cumsum[static_cast<std::size_t>(std::clamp(index_begin, int64_t{0}, last))] : 0.0;
    return end_value - begin_value;
}

inline std::vector<double> cumulative_sum(const std::vector<double>& values) {
    std::vector<double> out(values.size());
    double running = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) { running += values[i]; out[i] = running; }
    return out;
}

// Kahan 补偿求和(与 pandas groupby.sum 的 group_sum 逐位一致): 桶内成交求和用它,
// 否则 naive += 与 pandas 每秒差 ~1e-9, 经 cumsum 大数相减放大成 vpin 的 ~1e-13。
inline void kahan_add(double& sum, double& comp, double value) {
    const double y = value - comp;
    const double t = sum + y;
    comp = (t - sum) - y;
    sum  = t;
}

inline int64_t to_bucket(int64_t ts) { return ts / GRID_US * GRID_US; }

// 桶末/BN mid: 从 ×1e8 的买一/卖一价算 mid。供批处理 bucket_last_mid 与流式引擎共用,
// 运算顺序固定((bp+ap)/2/1e8), 不得改动否则破坏 bit-exact。
inline double ob_mid_scaled(int64_t bid_px0, int64_t ask_px0) {
    return (static_cast<double>(bid_px0) + static_cast<double>(ask_px0)) / 2.0 / PX_SCALE;
}

struct ImbSpread { double imb1, imb5, spread; };

// 从一个盘口(价已/1e8, 量已/1e8)算 imb1/imb5/spread。供 fill_imbalance_spread 与流式引擎共用,
// 运算顺序与原 fill_imbalance_spread 逐字一致, 不得改动。
inline ImbSpread imb_spread_from_levels(double bid0, double ask0,
                                        double bid_sz1, double ask_sz1,
                                        double bid_sz5, double ask_sz5) {
    const double mid    = (bid0 + ask0) / 2.0;
    const double denom1 = bid_sz1 + ask_sz1;
    const double denom5 = bid_sz5 + ask_sz5;
    ImbSpread r{0.0, 0.0, 0.0};
    if (denom1 > 0) r.imb1   = (bid_sz1 - ask_sz1) / denom1;
    if (denom5 > 0) r.imb5   = (bid_sz5 - ask_sz5) / denom5;
    if (mid    > 0) r.spread = (ask0 - bid0) / mid * 1e4;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────
//  1s 网格构建(单天)
// ─────────────────────────────────────────────────────────────────────────

// 全 OB 行的快照序列(ts 升序), 用于 imb/spread 的 as-of 查询。
struct ObSnapshots {
    std::vector<int64_t> ts;
    std::vector<double>  bid_px0, ask_px0;                 // 已 /1e8
    std::vector<double>  bid_size_top1, ask_size_top1;     // 第1档量(已 /1e8)
    std::vector<double>  bid_size_top5, ask_size_top5;     // 前5档量之和(已 /1e8)
};

inline ObSnapshots extract_ob_snapshots(const RawTable& table) {
    ObSnapshots snap;
    for (std::size_t i = 0; i < table.rows(); ++i) {
        if (table.price[i] != 0) continue;          // 仅 OB 行
        snap.ts.push_back(table.ts[i]);
        snap.bid_px0.push_back(static_cast<double>(table.bid_px0[i]) / PX_SCALE);
        snap.ask_px0.push_back(static_cast<double>(table.ask_px0[i]) / PX_SCALE);
        snap.bid_size_top1.push_back(table.bid_sz[0][i] / PX_SCALE);
        snap.ask_size_top1.push_back(table.ask_sz[0][i] / PX_SCALE);
        double bid5 = 0.0, ask5 = 0.0;
        for (int level = 0; level < 5; ++level) {
            bid5 += table.bid_sz[level][i] / PX_SCALE;
            ask5 += table.ask_sz[level][i] / PX_SCALE;
        }
        snap.bid_size_top5.push_back(bid5);
        snap.ask_size_top5.push_back(ask5);
    }
    return snap;  // 输入按 ts 升序 → snap.ts 升序
}

// 单天 1s 网格: mid=桶末OB, signed/abs_volume=桶内成交和, imb/spread=秒首as-of OB。
struct DayGrid {
    std::vector<int64_t> ts_sec;
    std::vector<double>  mid, signed_volume, abs_volume, imbalance_top1, imbalance_top5, spread_bps;
};

// 桶末 mid(groupby ts_sec last): 遍历 ts 升序 OB, 同秒覆盖为最后一个。
inline void bucket_last_mid(const RawTable& table,
                            std::vector<int64_t>& bucket, std::vector<double>& mid) {
    for (std::size_t i = 0; i < table.rows(); ++i) {
        if (table.price[i] != 0) continue;
        const int64_t sec = to_bucket(table.ts[i]);
        const double m = ob_mid_scaled(table.bid_px0[i], table.ask_px0[i]);
        if (!bucket.empty() && bucket.back() == sec) mid.back() = m;
        else { bucket.push_back(sec); mid.push_back(m); }
    }
}

// 桶内成交聚合: signed_volume=Σ(sign·usd), abs_volume=Σusd; sign=买+1/卖-1, usd=price·amount。
// 每个秒桶内用 Kahan 求和(匹配 pandas groupby.sum), 桶切换时落定上一桶并重置补偿。
inline void bucket_sum_trades(const RawTable& table, std::vector<int64_t>& bucket,
                              std::vector<double>& signed_vol, std::vector<double>& abs_vol) {
    double  sv = 0.0, sv_comp = 0.0, av = 0.0, av_comp = 0.0;
    int64_t bucket_cur = 0;
    bool    open = false;
    auto flush = [&] { bucket.push_back(bucket_cur); signed_vol.push_back(sv); abs_vol.push_back(av); };
    for (std::size_t i = 0; i < table.rows(); ++i) {
        if (table.price[i] == 0) continue;
        const int64_t sec  = to_bucket(table.ts[i]);
        const double  usd  = table.amount[i] * static_cast<double>(table.price[i]) / PX_SCALE;
        const double  sign = (table.side[i] == 0) ? 1.0 : -1.0;
        if (open && sec == bucket_cur) {
            kahan_add(sv, sv_comp, sign * usd);
            kahan_add(av, av_comp, usd);
        } else {
            if (open) flush();
            bucket_cur = sec; open = true;
            sv = sv_comp = av = av_comp = 0.0;
            kahan_add(sv, sv_comp, sign * usd);
            kahan_add(av, av_comp, usd);
        }
    }
    if (open) flush();
}

// imb/spread: 对每个网格秒起点, 取 ts≤秒起点 的最后 OB(与桶末 mid 不同快照)。
inline void fill_imbalance_spread(const ObSnapshots& snap, DayGrid& grid) {
    const std::size_t n = grid.ts_sec.size();
    grid.imbalance_top1.assign(n, 0.0);
    grid.imbalance_top5.assign(n, 0.0);
    grid.spread_bps.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t idx = prev_index(snap.ts, grid.ts_sec[i]);
        if (idx < 0) continue;
        const auto k = static_cast<std::size_t>(idx);
        const ImbSpread is = imb_spread_from_levels(
            snap.bid_px0[k], snap.ask_px0[k],
            snap.bid_size_top1[k], snap.ask_size_top1[k],
            snap.bid_size_top5[k], snap.ask_size_top5[k]);
        grid.imbalance_top1[i] = is.imb1;
        grid.imbalance_top5[i] = is.imb5;
        grid.spread_bps[i]     = is.spread;
    }
}

// 单天 OKX 网格 = 桶末 mid + 桶内成交(left join 到有OB的秒)+ imb/spread(秒首as-of)。
inline DayGrid build_okx_day_grid(const RawTable& table) {
    DayGrid grid;
    bucket_last_mid(table, grid.ts_sec, grid.mid);
    std::vector<int64_t> trade_sec;
    std::vector<double>  trade_signed, trade_abs;
    bucket_sum_trades(table, trade_sec, trade_signed, trade_abs);

    const std::size_t n = grid.ts_sec.size();
    grid.signed_volume.assign(n, 0.0);
    grid.abs_volume.assign(n, 0.0);
    std::size_t t = 0;                                // 双指针 left join(均按 ts_sec 升序)
    for (std::size_t i = 0; i < n; ++i) {
        while (t < trade_sec.size() && trade_sec[t] < grid.ts_sec[i]) ++t;
        if (t < trade_sec.size() && trade_sec[t] == grid.ts_sec[i]) {
            grid.signed_volume[i] = trade_signed[t];
            grid.abs_volume[i]    = trade_abs[t];
        }
    }
    fill_imbalance_spread(extract_ob_snapshots(table), grid);
    return grid;
}

// BN 侧只需桶末 mid。
inline DayGrid build_bn_mid_grid(const RawTable& table) {
    DayGrid grid;
    bucket_last_mid(table, grid.ts_sec, grid.mid);
    return grid;
}

// ─────────────────────────────────────────────────────────────────────────
//  跨天合并(concat + 排序 + 同秒取最后, 等价 pandas groupby last)
// ─────────────────────────────────────────────────────────────────────────

// 把多天 OKX 网格合并去重, 输出按 ts_sec 升序、每秒唯一。
inline DayGrid merge_okx_days(const std::vector<DayGrid>& days) {
    struct Row { int64_t ts_sec; double mid, sv, av, i1, i5, sp; std::size_t order; };
    std::vector<Row> rows;
    std::size_t order = 0;
    for (const auto& g : days)
        for (std::size_t i = 0; i < g.ts_sec.size(); ++i)
            rows.push_back({g.ts_sec[i], g.mid[i], g.signed_volume[i], g.abs_volume[i],
                            g.imbalance_top1[i], g.imbalance_top5[i], g.spread_bps[i], order++});
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.ts_sec < b.ts_sec; });
    DayGrid out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const bool last_of_sec = (i + 1 == rows.size()) || (rows[i + 1].ts_sec != rows[i].ts_sec);
        if (!last_of_sec) continue;                  // 同秒只保留最后一行(concat 顺序的最后)
        const Row& r = rows[i];
        out.ts_sec.push_back(r.ts_sec); out.mid.push_back(r.mid);
        out.signed_volume.push_back(r.sv); out.abs_volume.push_back(r.av);
        out.imbalance_top1.push_back(r.i1); out.imbalance_top5.push_back(r.i5);
        out.spread_bps.push_back(r.sp);
    }
    return out;
}

inline void merge_bn_days(const std::vector<DayGrid>& days,
                          std::vector<int64_t>& ts_out, std::vector<double>& mid_out) {
    struct Row { int64_t ts_sec; double mid; };
    std::vector<Row> rows;
    for (const auto& g : days)
        for (std::size_t i = 0; i < g.ts_sec.size(); ++i) rows.push_back({g.ts_sec[i], g.mid[i]});
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.ts_sec < b.ts_sec; });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const bool last_of_sec = (i + 1 == rows.size()) || (rows[i + 1].ts_sec != rows[i].ts_sec);
        if (!last_of_sec) continue;
        ts_out.push_back(rows[i].ts_sec); mid_out.push_back(rows[i].mid);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  pdiff(OKX-BN 基差)与对数收益
// ─────────────────────────────────────────────────────────────────────────

inline std::vector<double> log_returns(const std::vector<double>& mid) {
    std::vector<double> lr(mid.size(), 0.0);
    for (std::size_t i = 1; i < mid.size(); ++i) {
        const double ratio = (mid[i - 1] > 0) ? mid[i] / mid[i - 1] : 1.0;
        lr[i] = std::log(ratio);
    }
    return lr;
}

// pdiff_t=(mid_okx - bn_mid_asof)/bn_mid·1e4; cnt=有效标志。无 BN 时全 0。
inline void compute_pdiff(const std::vector<int64_t>& ts, const std::vector<double>& mid,
                          const std::vector<int64_t>& bn_ts, const std::vector<double>& bn_mid,
                          std::vector<double>& pdiff, std::vector<double>& count) {
    pdiff.assign(ts.size(), 0.0);
    count.assign(ts.size(), 0.0);
    if (bn_ts.empty()) return;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const std::int64_t bi = prev_index(bn_ts, ts[i]);
        if (bi < 0) continue;
        const double bn_m = bn_mid[static_cast<std::size_t>(bi)];
        const bool safe = (bn_m > 0) && std::isfinite(mid[i]) && (mid[i] > 0);
        if (!safe) continue;
        pdiff[i] = (mid[i] - bn_m) / bn_m * 1e4;
        count[i] = 1.0;
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  核心: 单 (symbol,date) 计算
// ─────────────────────────────────────────────────────────────────────────

// "YYYYMMDD" → 当天起点微秒(UTC); 失败返回 false。
inline bool day_bounds_us(std::string_view date, int64_t& lo, int64_t& hi) {
    std::tm tm{};
    const std::string text(date);
    if (strptime(text.c_str(), "%Y%m%d", &tm) == nullptr) return false;
    const std::time_t epoch = timegm(&tm);
    if (epoch == static_cast<std::time_t>(-1)) return false;
    lo = static_cast<int64_t>(epoch) * GRID_US;
    hi = lo + DAY_US;
    return true;
}

inline Features compute_day(const std::vector<RawTable>& okx_days,
                            const std::vector<RawTable>& bn_days,
                            std::string_view target_date) {
    int64_t target_lo = 0, target_hi = 0;
    if (!day_bounds_us(target_date, target_lo, target_hi)) {
        spdlog::error("compute_day: 非法日期 {}", target_date);
        return {};
    }

    std::vector<DayGrid> okx_grids;
    for (const auto& day : okx_days)
        if (day.rows() > 0) okx_grids.push_back(build_okx_day_grid(day));
    if (okx_grids.empty()) { spdlog::warn("compute_day[{}]: 无 OKX 数据", target_date); return {}; }

    std::vector<DayGrid> bn_grids;
    for (const auto& day : bn_days)
        if (day.rows() > 0) bn_grids.push_back(build_bn_mid_grid(day));

    const DayGrid grid = merge_okx_days(okx_grids);
    const std::vector<int64_t>& ts = grid.ts_sec;
    const std::vector<double>&  mid = grid.mid;
    spdlog::debug("compute_day[{}]: 合并网格 {} 秒", target_date, ts.size());

    std::vector<int64_t> bn_ts; std::vector<double> bn_mid;
    merge_bn_days(bn_grids, bn_ts, bn_mid);

    const std::vector<double> lr        = log_returns(mid);
    const std::vector<double> cs_svol   = cumulative_sum(grid.signed_volume);
    const std::vector<double> cs_avol   = cumulative_sum(grid.abs_volume);
    std::vector<double> lr2(lr.size());
    for (std::size_t i = 0; i < lr.size(); ++i) lr2[i] = lr[i] * lr[i];
    const std::vector<double> cs_lr2    = cumulative_sum(lr2);

    std::vector<double> pdiff, pcount;
    compute_pdiff(ts, mid, bn_ts, bn_mid, pdiff, pcount);
    const std::vector<double> cs_pdiff  = cumulative_sum(pdiff);
    const std::vector<double> cs_pcnt   = cumulative_sum(pcount);

    Features out;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const int64_t q = ts[i];
        if (q < target_lo || q >= target_hi) continue;          // 仅输出目标日

        const double svol60 = rolling_sum_asof(cs_svol, ts, q, 60);
        const double avol60 = rolling_sum_asof(cs_avol, ts, q, 60);
        const double svol10 = rolling_sum_asof(cs_svol, ts, q, 10);
        const double avol10 = rolling_sum_asof(cs_avol, ts, q, 10);
        const double lr2_300 = rolling_sum_asof(cs_lr2, ts, q, 300);

        const double mid_now = mid[i];
        const double mid_60  = grid_value_asof(ts, mid, q - 60  * GRID_US);
        const double mid_300 = grid_value_asof(ts, mid, q - 300 * GRID_US);

        const double svol_ratio = (avol60 > 1e-9) ? svol60 / avol60 : 0.0;
        const double vpin       = (avol10 > 1e-9) ? std::abs(svol10) / avol10 : -1.0;
        const double trend60    = (mid_now > 0) ? (mid_now - mid_60)  / mid_now * 1e4 : 0.0;
        const double trend300   = (mid_now > 0) ? (mid_now - mid_300) / mid_now * 1e4 : 0.0;
        const double rv_300s    = std::sqrt(std::max(lr2_300, 0.0)) * 1e4;

        const double pm_2h_s = rolling_sum_asof(cs_pdiff, ts, q, 7200);
        const double pm_2h_n = rolling_sum_asof(cs_pcnt,  ts, q, 7200);
        const double pm_2h   = (pm_2h_n > 0) ? pm_2h_s / pm_2h_n : 0.0;
        const double pm_12h_s = rolling_sum_asof(cs_pdiff, ts, q, 43200);
        const double pm_12h_n = rolling_sum_asof(cs_pcnt,  ts, q, 43200);
        const double pm_12h   = (pm_12h_n > 0) ? pm_12h_s / pm_12h_n : 0.0;

        out.ts_us.push_back(q);
        out.f0.push_back(grid.imbalance_top1[i]);
        out.f1.push_back(grid.imbalance_top5[i]);
        out.f2.push_back(grid.spread_bps[i]);
        out.f3.push_back(svol_ratio);
        out.f4.push_back(vpin);
        out.f5.push_back(trend60);
        out.f6.push_back(trend300);
        out.f7.push_back(rv_300s);
        out.f8.push_back(pm_2h);
        out.f9.push_back(pm_12h);
        out.mid_price.push_back(mid_now);
    }
    spdlog::info("compute_day[{}]: 输出 {} 行", target_date, out.rows());
    return out;
}

} // namespace tick_feat
