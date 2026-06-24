// main.cpp — tick_feat 离线批处理复刻 CLI(对应 build_tick_feat_standalone.py)
//
//   tick_feat --raw_dir <DIR> --symbol ADAUSDT --date 20260512 [--warmup_days 2] \
//             [--okx_prefix okx_swap] [--bn_prefix binance_swap] [--out ref_cpp.parquet]
//
// 读 raw_dir 下 {prefix}_{symbol}_{YYYYMMDD}.parquet(含暖机前日)→ compute_day → 写 parquet。
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#ifdef USE_PARQUET
  #include "parquet_io.hpp"
  inline constexpr const char* RAW_EXT = ".parquet";
#else
  #include "csv_io.hpp"             // 默认: 零依赖 CSV 路线(系统 arrow 不可用时)
  inline constexpr const char* RAW_EXT = ".csv";
#endif

namespace fs = std::filesystem;
using namespace tick_feat;

namespace {

struct Args {
    std::string raw_dir, symbol, date;
    std::string okx_prefix = "okx_swap";
    std::string bn_prefix  = "binance_swap";
    std::string out;                                    // 空 → main 按 IO 类型补默认扩展名
    int         warmup_days = 2;
};

std::string date_add(const std::string& date, int days) {
    std::tm tm{};
    strptime(date.c_str(), "%Y%m%d", &tm);
    std::time_t epoch = timegm(&tm) + static_cast<std::time_t>(days) * 86400;
    std::tm out{};
    gmtime_r(&epoch, &out);
    char buffer[16];
    std::strftime(buffer, sizeof buffer, "%Y%m%d", &out);
    return std::string(buffer);
}

std::optional<Args> parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i], value = argv[i + 1];
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
        spdlog::error("必填: --raw_dir --symbol --date");
        return std::nullopt;
    }
    return a;
}

// 加载某 prefix 的暖机+当天 parquet 为 RawTable 列表(缺文件跳过)。
std::vector<RawTable> load_days(const Args& a, const std::string& prefix,
                                const std::vector<std::string>& dates) {
    std::vector<RawTable> tables;
    for (const auto& day : dates) {
        const fs::path path = fs::path(a.raw_dir) / (prefix + "_" + a.symbol + "_" + day + RAW_EXT);
        if (!fs::exists(path)) { spdlog::debug("跳过缺失 {}", path.string()); continue; }
        auto table = read_raw_table(path.string());
        if (!table) { spdlog::warn("读取失败 {}: {}", path.string(), table.error()); continue; }
        tables.push_back(std::move(*table));
    }
    return tables;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_pattern("%Y-%m-%d %H:%M:%S %^%l%$ %v");
    const auto parsed = parse_args(argc, argv);
    if (!parsed) return 2;
    const Args& a = *parsed;

    std::vector<std::string> load_dates;                 // 暖机前日 → 当天
    for (int i = a.warmup_days; i >= 0; --i) load_dates.push_back(date_add(a.date, -i));
    spdlog::info("tick_feat[{} {}]: 加载 {} 天(含 {} 天暖机)", a.symbol, a.date,
                 load_dates.size(), a.warmup_days);

    const auto okx_days = load_days(a, a.okx_prefix, load_dates);
    const auto bn_days  = load_days(a, a.bn_prefix,  load_dates);
    if (okx_days.empty()) { spdlog::error("无 OKX 数据, 退出"); return 1; }

    const Features feat = compute_day(okx_days, bn_days, a.date);
    if (feat.rows() == 0) { spdlog::error("compute_day 输出为空"); return 1; }

    const std::string out = a.out.empty() ? std::string("tick_feat_ref_cpp") + RAW_EXT : a.out;
    const auto written = write_features(out, feat);
    if (!written) { spdlog::error("写出失败: {}", written.error()); return 1; }

    std::printf("OK rows=%zu -> %s\n", feat.rows(), out.c_str());
    for (std::size_t i = 0; i < std::min<std::size_t>(3, feat.rows()); ++i)
        std::printf("  ts_us=%ld f0=%.6g f1=%.6g f2=%.6g mid=%.6g\n",
                    feat.ts_us[i], feat.f0[i], feat.f1[i], feat.f2[i], feat.mid_price[i]);
    return 0;
}
