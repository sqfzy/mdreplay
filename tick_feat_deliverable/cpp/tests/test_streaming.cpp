// test_streaming.cpp — 流式引擎测试
//
// 核心验证: 同一合成 RawTable, 流式引擎(逐事件) 与 批处理 compute_day 的输出
// **逐位相同(bit-identical, ==)**。合成数据覆盖: 首秒(lr=0) / 桶末 mid 覆盖 /
// Kahan 多笔成交 / 纯成交秒丢弃 / 空秒跳过 / bn pdiff / 秒首 as-of(boundary)。
// compute_day 已对齐 python(0 diff), 故"流式==compute_day"即证流式正确。
#include <array>
#include <cstdio>

#include "live/replay_feed.hpp"
#include "live/streaming_engine.hpp"
#include "tick_feat.hpp"

using namespace tick_feat;
using namespace tick_feat::live;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// 合成标准 RawTable
struct TableBuilder {
    RawTable t;
    void ob(int64_t ts, int64_t bid_px0, int64_t ask_px0,
            std::array<double, 5> bsz, std::array<double, 5> asz) {
        t.ts.push_back(ts); t.side.push_back(0); t.price.push_back(0); t.amount.push_back(0.0);
        t.bid_px0.push_back(bid_px0); t.ask_px0.push_back(ask_px0);
        for (int l = 0; l < 5; ++l) { t.bid_sz[l].push_back(bsz[l]); t.ask_sz[l].push_back(asz[l]); }
    }
    void trade(int64_t ts, int64_t side, int64_t price, double amount) {
        t.ts.push_back(ts); t.side.push_back(side); t.price.push_back(price); t.amount.push_back(amount);
        t.bid_px0.push_back(0); t.ask_px0.push_back(0);
        for (int l = 0; l < 5; ++l) { t.bid_sz[l].push_back(0.0); t.ask_sz[l].push_back(0.0); }
    }
};

// 复用正式 replay_feed 把合成 RawTable 喂引擎
static Features run_stream(const RawTable& okx, const RawTable& bn) {
    StreamingFeatureEngine eng;
    replay(okx, bn, eng);
    return eng.features();
}

static void check_bit_identical(const Features& a, const Features& b) {
    CHECK(a.rows() == b.rows());
    if (a.rows() != b.rows()) return;
    const std::vector<double>* fa[11] = {&a.f0,&a.f1,&a.f2,&a.f3,&a.f4,&a.f5,&a.f6,&a.f7,&a.f8,&a.f9,&a.mid_price};
    const std::vector<double>* fb[11] = {&b.f0,&b.f1,&b.f2,&b.f3,&b.f4,&b.f5,&b.f6,&b.f7,&b.f8,&b.f9,&b.mid_price};
    for (std::size_t r = 0; r < a.rows(); ++r) {
        CHECK(a.ts_us[r] == b.ts_us[r]);
        for (int f = 0; f < 11; ++f) {
            const double x = (*fa[f])[r], y = (*fb[f])[r];
            const bool ok = (x == y) || (std::isnan(x) && std::isnan(y));   // bit-identical(nan==nan 视为相等)
            if (!ok) { std::printf("FAIL row=%zu f%d: %.17g != %.17g\n", r, f, x, y); ++failures; }
        }
    }
}

static void test_stream_eq_batch() {
    int64_t lo = 0, hi = 0;
    CHECK(day_bounds_us("20260101", lo, hi));
    const int64_t S = GRID_US;

    TableBuilder okx;
    // 秒0: 秒首 OB + 一笔买单
    okx.ob(lo,            10'000'000'000, 10'002'000'000, {10,0,0,0,0}, {5,0,0,0,0});
    okx.trade(lo+500'000, 0, 10'001'000'000, 2.0);
    // 秒1: 两个 OB(桶末覆盖) + 多笔成交(Kahan, 买卖混)
    okx.ob(lo+S,          10'001'000'000, 10'003'000'000, {8,1,0,0,0}, {6,2,0,0,0});
    okx.ob(lo+S+1,        10'002'000'000, 10'004'000'000, {7,0,0,0,0}, {7,0,0,0,0});
    okx.trade(lo+S+400'000, 1, 10'002'000'000, 3.0);
    okx.trade(lo+S+600'000, 0, 10'003'000'000, 1.5);
    // 秒2: 纯成交无 OB → 整秒丢弃
    okx.trade(lo+2*S+100'000, 0, 10'003'000'000, 9.0);
    // 秒4: OB(跳过秒3 空秒); 秒首正好整秒有 OB(测 ts==秒起点 boundary)
    okx.ob(lo+4*S,        10'005'000'000, 10'007'000'000, {4,0,0,0,0}, {9,0,0,0,0});
    okx.trade(lo+4*S+500'000, 1, 10'006'000'000, 2.5);

    TableBuilder bn;
    bn.ob(lo,       9'990'000'000, 9'992'000'000, {0,0,0,0,0}, {0,0,0,0,0});
    bn.ob(lo+S,     9'995'000'000, 9'997'000'000, {0,0,0,0,0}, {0,0,0,0,0});
    bn.ob(lo+4*S,   9'998'000'000, 10'000'000'000, {0,0,0,0,0}, {0,0,0,0,0});

    const Features batch  = compute_day({okx.t}, {bn.t}, "20260101");
    const Features stream = run_stream(okx.t, bn.t);

    CHECK(batch.rows() == 3);   // 秒0/1/4 有 OB; 秒2(纯成交)、秒3(空)不产行
    check_bit_identical(batch, stream);
}

// 无 BN 时 pdiff/pm 应为 0, 且流式仍==批处理
static void test_no_bn() {
    int64_t lo = 0, hi = 0; day_bounds_us("20260101", lo, hi);
    TableBuilder okx;
    okx.ob(lo, 10'000'000'000, 10'002'000'000, {10,0,0,0,0}, {5,0,0,0,0});
    okx.trade(lo+500'000, 0, 10'001'000'000, 2.0);
    const Features batch  = compute_day({okx.t}, {}, "20260101");
    const Features stream = run_stream(okx.t, RawTable{});
    check_bit_identical(batch, stream);
    CHECK(stream.rows() == 1 && stream.f8[0] == 0.0 && stream.f9[0] == 0.0);
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    test_stream_eq_batch();
    test_no_bn();
    if (failures) { std::printf("\n%d check(s) FAILED\n", failures); return 1; }
    std::printf("all tests passed\n");
    return 0;
}
