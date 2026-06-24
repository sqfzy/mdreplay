// main.cpp — mdreplay CLI:录制行情回放 → gconf v2 shm。
//
// 编排(函数即目录):解析参数 → 加载配置 → 开输出段 → 载入并归并输入 → 按节奏回放分发。
// CLI flag 覆盖 config:--config / --realtime / --dir / --start / --end。

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
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

// 解析 CLI,把覆盖项折进 cfg。返回 config 路径错误时 nullopt。
[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  std::string                config_path = "config.toml";
  std::optional<double>      ov_realtime;
  std::optional<std::string> ov_dir, ov_start, ov_end;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
    if (a == "--config") config_path = next();
    else if (a == "--realtime") ov_realtime = std::atof(next().c_str());
    else if (a == "--dir") ov_dir = next();
    else if (a == "--start") ov_start = next();
    else if (a == "--end") ov_end = next();
    else { spdlog::error("unknown arg: {}", a); return std::nullopt; }
  }

  auto cfg = mdreplay::load_config(config_path);
  if (!cfg) {
    spdlog::error("load config '{}' failed: {}", config_path, mdreplay::to_string(cfg.error()));
    return std::nullopt;
  }
  if (ov_realtime) cfg->realtime = *ov_realtime;
  if (ov_dir) cfg->dir = *ov_dir;
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
