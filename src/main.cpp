// main.cpp — mdreplay CLI:录制行情回放 → gconf v2 shm。
//
// 编排(函数即目录):解析参数 → 加载配置 → 开输出段 → 载入并归并输入 → 按节奏回放分发。
// CLI flag 覆盖 config:--config / --realtime / --dir / --start / --end。

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

#include <gconf/domain/symbols.h>
#include <gconf/shm/v2/board.h>
#include <gconf/shm/v2/trade.h>

#include "clock.hpp"
#include "config.hpp"
#include "input/csv_source.hpp"
#include "input/discover.hpp"
#include "merge.hpp"
#include "output/book_sink.hpp"
#include "output/shm_segment.hpp"
#include "output/trade_sink.hpp"
#include "record.hpp"
#include "report.hpp"

namespace v2 = gconf::shm::v2;
using mdreplay::Config;
using mdreplay::Error;
using mdreplay::Result;

namespace {

void print_usage() {
  std::puts(
      "mdreplay — 录制行情回放 → gconf v2 shm(下列选项覆盖 config.toml 同名项)\n"
      "  --config <path>                  配置文件(默认 config.toml)\n"
      "  --format <csv|json>              输入解析格式 (input.format)\n"
      "  --dir <dir>                      录制根目录 (input.dir)\n"
      "  --realtime <0~1>                 节奏:1=原速,0.5=2×,0=尽快可复现\n"
      "  --start \"YYYY-MM-DD HH:MM:SS\"     时间窗起(UTC,留空=最早)\n"
      "  --end   \"YYYY-MM-DD HH:MM:SS\"     时间窗止(UTC,闭区间)\n"
      "  --output.<i>.format <book|trade> 第 i 个 [[output]] 的 format\n"
      "  --output.<i>.shm <name>          第 i 个 [[output]] 的段名\n"
      "  --output.<i>.create <true|false> 第 i 个 [[output]] 建段/attach\n"
      "  --log-level <trace|debug|info|warn|error>\n"
      "  --progress-sec <n>               进度日志间隔秒\n"
      "  --help                           显示本帮助");
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view s) {
  if (s == "true" || s == "1") return true;
  if (s == "false" || s == "0") return false;
  return std::nullopt;
}

// 把 "--output.<i>.<key> <val>" 覆盖到 cfg.outputs[idx](镜像 [[output]] 数组路径)。
[[nodiscard]] bool apply_output_override(Config& cfg, std::size_t idx, std::string_view key,
                                         const std::string& val) {
  if (idx >= cfg.outputs.size()) {
    spdlog::error("--output.{}.* 越界:config 仅有 {} 个 output", idx, cfg.outputs.size());
    return false;
  }
  auto& o = cfg.outputs[idx];
  if (key == "shm") {
    o.shm = val;
  } else if (key == "format") {
    o.format = val;
  } else if (key == "create") {
    const auto b = parse_bool(val);
    if (!b) { spdlog::error("--output.{}.create 需 true/false", idx); return false; }
    o.create = *b;
  } else {
    spdlog::error("--output.{}.{} 未知键(应为 format/shm/create)", idx, key);
    return false;
  }
  return true;
}

// 解析 CLI,把覆盖项折进 cfg。--help 直接退出;解析/配置错误返回 nullopt。
[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  std::string                config_path = "config.toml";
  std::optional<std::string> ov_format, ov_dir, ov_start, ov_end, ov_log;
  std::optional<double>      ov_realtime;
  std::optional<int>         ov_progress;
  std::vector<std::tuple<std::size_t, std::string, std::string>> out_ovr;  // (idx, key, val)

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
    if (a == "--help") { print_usage(); std::exit(0); }
    else if (a == "--config") config_path = next();
    else if (a == "--format") ov_format = next();
    else if (a == "--dir") ov_dir = next();
    else if (a == "--realtime") ov_realtime = std::atof(next().c_str());
    else if (a == "--start") ov_start = next();
    else if (a == "--end") ov_end = next();
    else if (a == "--log-level") ov_log = next();
    else if (a == "--progress-sec") ov_progress = std::atoi(next().c_str());
    else if (a.starts_with("--output.")) {              // --output.<i>.<key>(镜像 [[output]] 数组)
      const std::string rest = a.substr(std::string_view("--output.").size());
      const auto        dot  = rest.find('.');
      if (dot == std::string::npos) {
        spdlog::error("用法 --output.<i>.<key>(得到 {})", a);
        return std::nullopt;
      }
      out_ovr.emplace_back(static_cast<std::size_t>(std::atoi(rest.substr(0, dot).c_str())),
                           rest.substr(dot + 1), next());
    }
    else { spdlog::error("unknown arg: {} (--help 查看用法)", a); return std::nullopt; }
  }

  auto cfg = mdreplay::load_config(config_path);
  if (!cfg) {
    spdlog::error("load config '{}' failed: {}", config_path, mdreplay::to_string(cfg.error()));
    return std::nullopt;
  }

  if (ov_format) cfg->input_format = *ov_format;
  if (ov_dir) cfg->dir = *ov_dir;
  if (ov_realtime) cfg->realtime = *ov_realtime;
  if (ov_log) cfg->log_level = *ov_log;
  if (ov_progress) cfg->progress_sec = *ov_progress;
  if (ov_start) {
    const auto v = mdreplay::parse_datetime_ns(*ov_start, mdreplay::kNoStart);
    if (!v) { spdlog::error("bad --start datetime"); return std::nullopt; }
    cfg->start_ns = *v;
  }
  if (ov_end) {
    const auto v = mdreplay::parse_datetime_ns(*ov_end, mdreplay::kNoEnd);
    if (!v) { spdlog::error("bad --end datetime"); return std::nullopt; }
    cfg->end_ns = *v;
  }
  for (const auto& [idx, key, val] : out_ovr)
    if (!apply_output_override(*cfg, idx, key, val)) return std::nullopt;

  // 覆盖后复校验(CLI 可能注入越界值,绕过 load_config 期的校验)
  if (cfg->realtime < 0.0 || cfg->realtime > 1.0) {
    spdlog::error("realtime 须 ∈ [0,1](得到 {})", cfg->realtime);
    return std::nullopt;
  }
  if (cfg->start_ns > cfg->end_ns) {
    spdlog::error("start > end");
    return std::nullopt;
  }
  return *cfg;
}

// 为每个 [[output]] 开 shm 段,持有段(保活 mmap)并产出对应 sink。
struct Sinks {
  std::vector<mdreplay::ShmSegment> segs;
  std::unique_ptr<mdreplay::Sink>   book, trade;
};

[[nodiscard]] Result<Sinks> open_sinks(const Config& cfg) {
  Sinks sinks;
  sinks.segs.reserve(cfg.outputs.size());
  for (const auto& o : cfg.outputs) {
    if (o.format == "book") {
      auto seg = mdreplay::ShmSegment::open(o.shm, sizeof(v2::Board), v2::SegKind::Board,
                                            sizeof(v2::BoardSlot), gconf::sym::N_GLOBAL_SYMBOL_IDS,
                                            v2::kBoardSchemaHash, o.create);
      if (!seg) return std::unexpected(seg.error());
      sinks.segs.push_back(std::move(*seg));
      sinks.book = std::make_unique<mdreplay::BookSink>(
          reinterpret_cast<v2::Board*>(sinks.segs.back().base()));
    } else {  // trade(config 已校验只有 book/trade)
      auto seg = mdreplay::ShmSegment::open(o.shm, sizeof(v2::TradeRing), v2::SegKind::BcastRing,
                                            sizeof(v2::TradeEntry), v2::TRADE_RING_CAP,
                                            v2::kTradeSchemaHash, o.create);
      if (!seg) return std::unexpected(seg.error());
      sinks.segs.push_back(std::move(*seg));
      sinks.trade = std::make_unique<mdreplay::TradeSink>(
          reinterpret_cast<v2::TradeRing*>(sinks.segs.back().base()));
    }
    spdlog::info("attach {:<5} {} ({})", o.format, o.shm, o.create ? "created" : "attached");
  }
  return sinks;
}

// 载入 dir 下所有 book/trade 文件为有序源;skipped 回填跳过行数。
[[nodiscard]] Result<std::vector<std::unique_ptr<mdreplay::Source>>>
load_sources(const Config& cfg, std::size_t& skipped) {
  std::vector<std::unique_ptr<mdreplay::Source>> sources;
  for (const auto& fi : mdreplay::discover(cfg.dir)) {
    auto s = mdreplay::load_csv_source(fi.path, fi.kind, skipped);
    if (!s) return std::unexpected(s.error());
    sources.push_back(std::move(*s));
  }
  return sources;
}

// 归并 → 按节奏 → 路由到 sink。
void replay(const Config& cfg, std::vector<std::unique_ptr<mdreplay::Source>> sources,
            Sinks& sinks, std::size_t skipped) {
  mdreplay::Merger   merger(std::move(sources), cfg.start_ns, cfg.end_ns);
  mdreplay::Clock    clock(cfg.realtime);
  mdreplay::Reporter reporter(cfg.progress_sec);
  reporter.add_skipped(skipped);

  while (const auto rec = merger.next()) {
    clock.pace_to(rec->ts_ns);
    if (rec->kind == mdreplay::Kind::Book) {
      if (sinks.book) (void)sinks.book->write(*rec);
    } else if (sinks.trade) {
      (void)sinks.trade->write(*rec);
    }
    reporter.on_event(*rec);
  }
  reporter.finish();
}

}  // namespace

int main(int argc, char** argv) {
  const auto cfg = load_with_overrides(argc, argv);
  if (!cfg) return 1;
  spdlog::set_level(spdlog::level::from_str(cfg->log_level));

  auto sinks = open_sinks(*cfg);
  if (!sinks) {
    spdlog::error("open output segments failed: {}", mdreplay::to_string(sinks.error()));
    return 1;
  }

  std::size_t skipped = 0;
  auto        sources = load_sources(*cfg, skipped);
  if (!sources) {
    spdlog::error("load input failed: {}", mdreplay::to_string(sources.error()));
    return 1;
  }
  if (sources->empty()) {
    spdlog::error("no *.book.csv / *.trade.csv under '{}'", cfg->dir);
    return 1;
  }
  spdlog::info("datas: {} sources, realtime={}, window=[{}..{}]", sources->size(), cfg->realtime,
               cfg->start_ns, cfg->end_ns);

  replay(*cfg, std::move(*sources), *sinks, skipped);
  return 0;
}
