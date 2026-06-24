// live_replay_main.cpp — 流式引擎 replay CLI
//
//   tick_feat_replay --raw_dir <DIR> --symbol SOLUSDT --date 20260623 --warmup_days 0
//                    [--okx_prefix okx_swap] [--bn_prefix binance_swap] [--out mine_stream.csv] [--verify]
//
// 读 formatted 标准行(OKX+BN) → 当历史流喂 StreamingFeatureEngine → 写 f0-f9 csv。
// --verify: 额外跑批处理 compute_day 同输入, 断言流式输出逐位相同(bit-identical)。
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#ifdef USE_PARQUET
  #include "parquet_io.hpp"
  inline constexpr const char* RAW_EXT = ".parquet";
#else
  #include "csv_io.hpp"
  inline constexpr const char* RAW_EXT = ".csv";
#endif

#include "live/replay_feed.hpp"
#include "live/streaming_engine.hpp"
#include "tick_feat.hpp"

namespace fs = std::filesystem;
using namespace tick_feat;
using namespace tick_feat::live;

namespace {

struct Args {
    std::string raw_dir, symbol, date;
    std::string okx_prefix = "okx_swap";
    std::string bn_prefix  = "binance_swap";
    std::string out;
    int         warmup_days = 0;
    bool        verify = false;
};

std::optional<Args> parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--verify") { a.verify = true; continue; }
        if (i + 1 >= argc) { spdlog::error("参数 {} 缺值", key); return std::nullopt; }
        const std::string value = argv[++i];
        if      (key == "--raw_dir")     a.raw_dir = value;
        else if (key == "--symbol")      a.symbol = value;
        else if (key == "--date")        a.date = value;
        else if (key == "--warmup_days") a.warmup_days = std::stoi(value);
        else if (key == "--okx_prefix")  a.okx_prefix = value;
        else if (key == "--bn_prefix")   a.bn_prefix = value;
        else if (key == "--out")         a.out = value;
        else { spdlog::error("未知参数: {}", key); return std::nullopt; }
    }
    if (a.raw_dir.empty() || a.symbol.empty() || a.date.empty()) {
        spdlog::error("必填: --raw_dir --symbol --date"); return std::nullopt;
    }
    if (a.warmup_days != 0) {
        spdlog::error("本版仅支持 --warmup_days 0(单 date 单文件); 多天暖机待后续"); return std::nullopt;
    }
    return a;
}

std::string raw_path(const Args& a, const std::string& prefix) {
    return (fs::path(a.raw_dir) / (prefix + "_" + a.symbol + "_" + a.date + RAW_EXT)).string();
}

// 逐位比较流式与批处理; 返回是否完全一致。
bool bit_identical(const Features& s, const Features& b) {
    if (s.rows() != b.rows()) { spdlog::error("行数不同: stream={} batch={}", s.rows(), b.rows()); return false; }
    const std::vector<double>* fs_[11] = {&s.f0,&s.f1,&s.f2,&s.f3,&s.f4,&s.f5,&s.f6,&s.f7,&s.f8,&s.f9,&s.mid_price};
    const std::vector<double>* fb_[11] = {&b.f0,&b.f1,&b.f2,&b.f3,&b.f4,&b.f5,&b.f6,&b.f7,&b.f8,&b.f9,&b.mid_price};
    std::size_t mism = 0;
    for (std::size_t r = 0; r < s.rows(); ++r) {
        if (s.ts_us[r] != b.ts_us[r]) { ++mism; continue; }
        for (int f = 0; f < 11; ++f) {
            const double x = (*fs_[f])[r], y = (*fb_[f])[r];
            if (!((x == y) || (std::isnan(x) && std::isnan(y)))) {
                if (mism < 5) spdlog::error("row={} f{}: {:.17g} != {:.17g}", r, f, x, y);
                ++mism;
            }
        }
    }
    if (mism) { spdlog::error("流式 vs 批处理: {} 处不一致", mism); return false; }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("%Y-%m-%d %H:%M:%S %^%l%$ %v");
    const auto parsed = parse_args(argc, argv);
    if (!parsed) return 2;
    const Args& a = *parsed;

    auto okx = read_raw_table(raw_path(a, a.okx_prefix));
    if (!okx) { spdlog::error("读 OKX 失败: {}", okx.error()); return 1; }
    RawTable bn_table;
    const std::string bn_p = raw_path(a, a.bn_prefix);
    if (fs::exists(bn_p)) {
        auto bn = read_raw_table(bn_p);
        if (bn) bn_table = std::move(*bn);
        else spdlog::warn("读 BN 失败(pdiff 将为 0): {}", bn.error());
    } else {
        spdlog::warn("无 BN 文件 {}, pdiff 将为 0", bn_p);
    }

    StreamingFeatureEngine engine;
    replay(*okx, bn_table, engine);
    const Features& feat = engine.features();
    if (feat.rows() == 0) { spdlog::error("流式输出为空"); return 1; }

    const std::string out = a.out.empty() ? std::string("tick_feat_stream") + RAW_EXT : a.out;
    const auto written = write_features(out, feat);
    if (!written) { spdlog::error("写出失败: {}", written.error()); return 1; }
    std::printf("OK rows=%zu -> %s\n", feat.rows(), out.c_str());

    if (a.verify) {
        std::vector<RawTable> bn_days;
        if (bn_table.rows()) bn_days.push_back(bn_table);
        const Features batch = compute_day({*okx}, bn_days, a.date);
        if (bit_identical(feat, batch))
            std::printf("VERIFY: 流式 == 批处理 compute_day (bit-identical, %zu 行) ✅\n", feat.rows());
        else { std::printf("VERIFY: 流式 != 批处理 ❌\n"); return 1; }
    }
    return 0;
}
