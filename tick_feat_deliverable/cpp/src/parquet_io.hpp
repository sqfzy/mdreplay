// parquet_io.hpp — 标准 64 列 parquet 读取 + Features 写出(Apache Arrow)
//
// 读: 按列索引(非列名)取标准布局, 兼容 int32/int64/float/double 存储变体。
// 写: ts_us(int64) + f0..f9(double) + mid_price(double)。
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>

#include "tick_feat.hpp"

namespace tick_feat {

// 任意整数/浮点列 → vector<int64_t>(逐 chunk 展开)。
inline std::vector<int64_t> column_to_i64(const std::shared_ptr<arrow::ChunkedArray>& col) {
    std::vector<int64_t> out;
    out.reserve(static_cast<std::size_t>(col->length()));
    for (int c = 0; c < col->num_chunks(); ++c) {
        const auto& chunk = *col->chunk(c);
        for (int64_t i = 0; i < chunk.length(); ++i) {
            switch (chunk.type_id()) {
                case arrow::Type::INT64:  out.push_back(static_cast<const arrow::Int64Array&>(chunk).Value(i)); break;
                case arrow::Type::INT32:  out.push_back(static_cast<const arrow::Int32Array&>(chunk).Value(i)); break;
                case arrow::Type::DOUBLE: out.push_back(static_cast<int64_t>(static_cast<const arrow::DoubleArray&>(chunk).Value(i))); break;
                case arrow::Type::FLOAT:  out.push_back(static_cast<int64_t>(static_cast<const arrow::FloatArray&>(chunk).Value(i))); break;
                default: out.push_back(0);
            }
        }
    }
    return out;
}

// 任意整数/浮点列 → vector<double>。
inline std::vector<double> column_to_f64(const std::shared_ptr<arrow::ChunkedArray>& col) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(col->length()));
    for (int c = 0; c < col->num_chunks(); ++c) {
        const auto& chunk = *col->chunk(c);
        for (int64_t i = 0; i < chunk.length(); ++i) {
            switch (chunk.type_id()) {
                case arrow::Type::DOUBLE: out.push_back(static_cast<const arrow::DoubleArray&>(chunk).Value(i)); break;
                case arrow::Type::FLOAT:  out.push_back(static_cast<const arrow::FloatArray&>(chunk).Value(i)); break;
                case arrow::Type::INT64:  out.push_back(static_cast<double>(static_cast<const arrow::Int64Array&>(chunk).Value(i))); break;
                case arrow::Type::INT32:  out.push_back(static_cast<double>(static_cast<const arrow::Int32Array&>(chunk).Value(i))); break;
                default: out.push_back(0.0);
            }
        }
    }
    return out;
}

inline std::expected<std::shared_ptr<arrow::Table>, std::string> read_table(const std::string& path) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) return std::unexpected("open 失败: " + infile_result.status().ToString());
    auto reader_result = parquet::arrow::OpenFile(*infile_result, arrow::default_memory_pool());
    if (!reader_result.ok()) return std::unexpected("parquet open 失败: " + reader_result.status().ToString());
    auto reader = std::move(*reader_result);
    std::shared_ptr<arrow::Table> table;
    auto status = reader->ReadTable(&table);
    if (!status.ok()) return std::unexpected("ReadTable 失败: " + status.ToString());
    return table;
}

// 读标准 64 列 parquet → RawTable(只取算法需要的列)。
inline std::expected<RawTable, std::string> read_raw_table(const std::string& path) {
    auto table_result = read_table(path);
    if (!table_result) return std::unexpected(table_result.error());
    const auto& table = *table_result;
    if (table->num_columns() < COL_ASK_SZ + 5)
        return std::unexpected("列数不足: " + std::to_string(table->num_columns()));

    RawTable raw;
    raw.ts      = column_to_i64(table->column(COL_TS));
    raw.side    = column_to_i64(table->column(COL_SIDE));
    raw.price   = column_to_i64(table->column(COL_PRICE));
    raw.amount  = column_to_f64(table->column(COL_AMOUNT));
    raw.bid_px0 = column_to_i64(table->column(COL_BID_PX));
    raw.ask_px0 = column_to_i64(table->column(COL_ASK_PX));
    for (int level = 0; level < 5; ++level) {
        raw.bid_sz[level] = column_to_f64(table->column(COL_BID_SZ + level));
        raw.ask_sz[level] = column_to_f64(table->column(COL_ASK_SZ + level));
    }
    spdlog::debug("read_raw_table: {} 行 <- {}", raw.rows(), path);
    return raw;
}

inline std::shared_ptr<arrow::Array> make_f64_array(const std::vector<double>& values) {
    arrow::DoubleBuilder builder;
    builder.AppendValues(values).ok();
    std::shared_ptr<arrow::Array> array;
    builder.Finish(&array).ok();
    return array;
}

// 写 Features → parquet(列: ts_us, f0..f9, mid_price)。
inline std::expected<void, std::string> write_features(const std::string& path, const Features& feat) {
    arrow::Int64Builder ts_builder;
    if (!ts_builder.AppendValues(feat.ts_us).ok()) return std::unexpected("ts append 失败");
    std::shared_ptr<arrow::Array> ts_array;
    if (!ts_builder.Finish(&ts_array).ok()) return std::unexpected("ts finish 失败");

    const auto schema = arrow::schema({
        arrow::field("ts_us", arrow::int64()),
        arrow::field("f0", arrow::float64()), arrow::field("f1", arrow::float64()),
        arrow::field("f2", arrow::float64()), arrow::field("f3", arrow::float64()),
        arrow::field("f4", arrow::float64()), arrow::field("f5", arrow::float64()),
        arrow::field("f6", arrow::float64()), arrow::field("f7", arrow::float64()),
        arrow::field("f8", arrow::float64()), arrow::field("f9", arrow::float64()),
        arrow::field("mid_price", arrow::float64()),
    });
    const auto table = arrow::Table::Make(schema, {
        ts_array,
        make_f64_array(feat.f0), make_f64_array(feat.f1), make_f64_array(feat.f2),
        make_f64_array(feat.f3), make_f64_array(feat.f4), make_f64_array(feat.f5),
        make_f64_array(feat.f6), make_f64_array(feat.f7), make_f64_array(feat.f8),
        make_f64_array(feat.f9), make_f64_array(feat.mid_price),
    });

    auto outfile_result = arrow::io::FileOutputStream::Open(path);
    if (!outfile_result.ok()) return std::unexpected("输出 open 失败: " + outfile_result.status().ToString());
    auto status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *outfile_result,
                                             /*chunk_size=*/std::max<int64_t>(1, table->num_rows()));
    if (!status.ok()) return std::unexpected("WriteTable 失败: " + status.ToString());
    spdlog::info("write_features: {} 行 -> {}", feat.rows(), path);
    return {};
}

} // namespace tick_feat
