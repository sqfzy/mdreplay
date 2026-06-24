// csv_io.hpp — 标准 64 列 CSV 读取 + Features 写出(零外部依赖)
//
// 与 parquet_io.hpp 提供同名接口(read_raw_table / write_features), main 二选一 include。
// 用途: 系统 arrow 不可用时, 用 python 端导出的标准 CSV 做端到端对拍。
// CSV 由 format_okx_jsonl.py --csv 产出(pyarrow write_csv, 带表头, 列序同标准布局)。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "tick_feat.hpp"

namespace tick_feat {

inline constexpr int CSV_COLS = 4 + 4 * N_LVLS;     // 64

// 读标准 64 列 CSV → RawTable(整数列 <2^53, double 解析后转 int64 无损)。
inline std::expected<RawTable, std::string> read_raw_table(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::unexpected("open 失败: " + path);
    std::string line;
    if (!std::getline(in, line)) return std::unexpected("空文件: " + path);  // 跳过表头

    RawTable table;
    std::vector<double> field(CSV_COLS);
    std::size_t bad_lines = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const char* cursor = line.c_str();
        bool ok = true;
        for (int c = 0; c < CSV_COLS; ++c) {
            char* end = nullptr;
            field[c] = std::strtod(cursor, &end);
            if (end == cursor) { ok = false; break; }
            cursor = (*end == ',') ? end + 1 : end;
        }
        if (!ok) { ++bad_lines; continue; }
        table.ts.push_back(static_cast<int64_t>(field[COL_TS]));
        table.side.push_back(static_cast<int64_t>(field[COL_SIDE]));
        table.price.push_back(static_cast<int64_t>(field[COL_PRICE]));
        table.amount.push_back(field[COL_AMOUNT]);
        table.bid_px0.push_back(static_cast<int64_t>(field[COL_BID_PX]));
        table.ask_px0.push_back(static_cast<int64_t>(field[COL_ASK_PX]));
        for (int level = 0; level < 5; ++level) {
            table.bid_sz[level].push_back(field[COL_BID_SZ + level]);
            table.ask_sz[level].push_back(field[COL_ASK_SZ + level]);
        }
    }
    if (bad_lines) spdlog::warn("read_raw_table(csv): 跳过 {} 行格式异常", bad_lines);
    spdlog::debug("read_raw_table(csv): {} 行 <- {}", table.rows(), path);
    return table;
}

// 写 Features → CSV(表头 + 数据); double 用 std::format 最短往返表示, nan 写 "nan"。
inline std::expected<void, std::string> write_features(const std::string& path, const Features& feat) {
    std::ofstream out(path);
    if (!out.is_open()) return std::unexpected("输出 open 失败: " + path);
    out << "ts_us,f0,f1,f2,f3,f4,f5,f6,f7,f8,f9,mid_price\n";
    for (std::size_t i = 0; i < feat.rows(); ++i) {
        out << std::format("{},{},{},{},{},{},{},{},{},{},{},{}\n",
                           feat.ts_us[i], feat.f0[i], feat.f1[i], feat.f2[i], feat.f3[i],
                           feat.f4[i], feat.f5[i], feat.f6[i], feat.f7[i], feat.f8[i],
                           feat.f9[i], feat.mid_price[i]);
    }
    if (!out) return std::unexpected("写入失败: " + path);
    spdlog::info("write_features(csv): {} 行 -> {}", feat.rows(), path);
    return {};
}

} // namespace tick_feat
