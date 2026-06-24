// test_core.cpp — tick_feat 算法核心纯逻辑单元测试(不依赖 parquet/arrow)
//
// 覆盖: 边界查询工具 + 一个可手算的 compute_day 小样本(校验关键口径,
// 尤其 "桶末 mid vs 秒首 imb 不同快照" 与 rolling/vpin/svol_ratio)。
#include <cmath>
#include <cstdio>
#include <vector>

#include "tick_feat.hpp"

using namespace tick_feat;

static int failures = 0;

#define CHECK(cond)                                                                       \
    do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

#define CHECK_NEAR(actual, expected, tol)                                                 \
    do {                                                                                  \
        const double diff = std::abs((actual) - (expected));                              \
        if (diff > (tol)) {                                                               \
            std::printf("FAIL %s:%d  |%.10g - %.10g| = %.3g > %g\n",                      \
                        __FILE__, __LINE__, (double)(actual), (double)(expected), diff, (double)(tol)); \
            ++failures;                                                                    \
        }                                                                                 \
    } while (0)

static void test_prev_index() {
    const std::vector<int64_t> grid = {10, 20, 30};
    CHECK(prev_index(grid, 5)  == -1);   // 早于首元素
    CHECK(prev_index(grid, 10) == 0);    // 命中(side=right)
    CHECK(prev_index(grid, 25) == 1);    // 落在 20 与 30 之间
    CHECK(prev_index(grid, 99) == 2);    // 晚于末元素
}

static void test_rolling_sum() {
    const std::vector<int64_t> ts = {0, GRID_US, 2 * GRID_US};        // 0s,1s,2s
    const std::vector<double>  cs = cumulative_sum({1.0, 2.0, 3.0});  // {1,3,6}
    // 固化 python _rolling_sum 语义: 下界 = prev_idx(q - window*1e6 - 1)。
    // 与网格点对齐时有既有 off-by-one: w=2@q=2s 下界落到 -1 → 含全部 = 6;
    // w=1@q=2s 下界 prev_idx(999999)=0 → begin=cs[0], end-begin = 6-1 = 5。
    CHECK_NEAR(rolling_sum_asof(cs, ts, 2 * GRID_US, 2), 6.0, 1e-12);
    CHECK_NEAR(rolling_sum_asof(cs, ts, 2 * GRID_US, 1), 5.0, 1e-12);
    CHECK_NEAR(rolling_sum_asof(cs, ts, 2 * GRID_US, 10), 6.0, 1e-12);  // 全覆盖
    CHECK_NEAR(rolling_sum_asof(cs, ts, -1, 60), 0.0, 1e-12);           // 无有效末点
}

// 单天单秒: 1 个 OB(秒起点) + 1 笔买单成交, 手算各特征。
static void test_compute_day_single_bucket() {
    int64_t lo = 0, hi = 0;
    CHECK(day_bounds_us("20260101", lo, hi));

    RawTable t;
    const int64_t bp0 = 10'000'000'000;   // 100.00 ×1e8
    const int64_t ap0 = 10'002'000'000;   // 100.02 ×1e8
    const int64_t tpx = 10'001'000'000;   // 100.01 ×1e8
    // row0: OB @ 秒起点; row1: 买单成交 @ 同秒中段
    t.ts     = {lo,            lo + 500'000};
    t.side   = {0,             0};
    t.price  = {0,             tpx};       // OB 行 price=0
    t.amount = {0.0,           2.0};
    t.bid_px0 = {bp0,          0};
    t.ask_px0 = {ap0,          0};
    for (int level = 0; level < 5; ++level) { t.bid_sz[level] = {0.0, 0.0}; t.ask_sz[level] = {0.0, 0.0}; }
    t.bid_sz[0] = {10.0, 0.0};             // 第1档买量 10
    t.ask_sz[0] = {5.0,  0.0};             // 第1档卖量 5

    std::vector<RawTable> okx = {t};
    const Features f = compute_day(okx, {}, "20260101");

    CHECK(f.rows() == 1);
    if (f.rows() != 1) return;
    CHECK_NEAR(f.mid_price[0], 100.01, 1e-9);                 // 桶末 mid
    CHECK_NEAR(f.f0[0], (10.0 - 5.0) / 15.0, 1e-12);         // imb_top1 = 1/3
    CHECK_NEAR(f.f1[0], (10.0 - 5.0) / 15.0, 1e-12);         // imb_top5 (仅L0非0, 同上)
    CHECK_NEAR(f.f2[0], (100.02 - 100.00) / 100.01 * 1e4, 1e-9);  // spread_bps
    CHECK_NEAR(f.f3[0], 1.0, 1e-12);                          // svol_ratio: 全买 → +1
    CHECK_NEAR(f.f4[0], 1.0, 1e-12);                          // vpin: |svol|/avol = 1
    CHECK(std::isnan(f.f5[0]));                               // trend60: q-60s 无网格 → nan
    CHECK(std::isnan(f.f6[0]));                               // trend300 同
    CHECK_NEAR(f.f7[0], 0.0, 1e-12);                          // rv: 首点 logret=0
    CHECK_NEAR(f.f8[0], 0.0, 1e-12);                          // pm_2h: 无 BN → 0
    CHECK_NEAR(f.f9[0], 0.0, 1e-12);
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    test_prev_index();
    test_rolling_sum();
    test_compute_day_single_bucket();
    if (failures) { std::printf("\n%d check(s) FAILED\n", failures); return 1; }
    std::printf("all tests passed\n");
    return 0;
}
