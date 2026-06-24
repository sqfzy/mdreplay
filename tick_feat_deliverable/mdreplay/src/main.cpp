// main.cpp — mdreplay CLI:单入单出回放(一次只一种 book 或 trade)→ gconf v2 shm。
//
// 刻意不耦合 book/trade:要两者都喂下游,跑两遍(改 --output.format 与 --output.shm)。
// 编排:解析参数 → 加载配置 → 开一个输出段 → 载入该类输入并归并 → 按节奏回放。

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
using mdreplay::Result;

namespace {

void print_usage() {
  std::puts(
      "mdreplay — 单入单出行情回放 → gconf v2 shm(选项覆盖 config.toml 同名项)\n"
      "一次只回放一种(book 或 trade);两者都要就跑两遍(改 --output.format 与 --output.shm)。\n"
      "  --config <path>                配置文件(默认 config.toml)\n"
      "  --format <csv|json>            输入解析格式 (input.format)\n"
      "  --dir <dir>                    录制根目录 (input.dir)\n"
      "  --realtime <0~1>               节奏:1=原速,0.5=2×,0=尽快可复现\n"
      "  --start \"YYYY-MM-DD HH:MM:SS\"   时间窗起(UTC,留空=最早)\n"
      "  --end   \"YYYY-MM-DD HH:MM:SS\"   时间窗止(UTC,闭区间)\n"
      "  --output.format <book|trade>   回放哪种(决定读 *.<format>.csv + 写哪种段)\n"
      "  --output.shm <name>            输出段名\n"
      "  --output.create <true|false>   建段(true)/attach(false)\n"
      "  --log-level <trace|debug|info|warn|error>\n"
      "  --progress-sec <n>             进度日志间隔秒\n"
      "  --help                         显示本帮助");
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view s) {
  if (s == "true" || s == "1") return true;
  if (s == "false" || s == "0") return false;
  return std::nullopt;
}

// 应用 "--output.<key> <val>" 覆盖到 cfg.output(镜像 [output] 表路径)。
[[nodiscard]] bool apply_output(Config& cfg, std::string_view key, const std::string& val) {
  if (key == "format") {
    cfg.output.format = val;
  } else if (key == "shm") {
    cfg.output.shm = val;
  } else if (key == "create") {
    const auto b = parse_bool(val);
    if (!b) { spdlog::error("--output.create 需 true/false"); return false; }
    cfg.output.create = *b;
  } else {
    spdlog::error("--output.{} 未知键(应为 format/shm/create)", key);
    return false;
  }
  return true;
}

// 解析 CLI,把覆盖项折进 cfg。--help 直接退出;解析/配置错误返回 nullopt。
[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  std::string                                      config_path = "config.toml";
  std::optional<std::string>                       ov_format, ov_dir, ov_start, ov_end, ov_log;
  std::optional<double>                            ov_realtime;
  std::optional<int>                               ov_progress;
  std::vector<std::pair<std::string, std::string>> out_ovr;  // (key, val)

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
    else if (a.starts_with("--output."))
      out_ovr.emplace_back(a.substr(std::string_view("--output.").size()), next());
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
  for (const auto& [key, val] : out_ovr)
    if (!apply_output(*cfg, key, val)) return std::nullopt;

  // 覆盖后复校验(CLI 可能注入越界/非法值,绕过 load_config 期校验)
  if (cfg->realtime < 0.0 || cfg->realtime > 1.0) {
    spdlog::error("realtime 须 ∈ [0,1](得到 {})", cfg->realtime);
    return std::nullopt;
  }
  if (cfg->start_ns > cfg->end_ns) { spdlog::error("start > end"); return std::nullopt; }
  if (cfg->output.format != "book" && cfg->output.format != "trade") {
    spdlog::error("output.format 须 book/trade(得到 '{}')", cfg->output.format);
    return std::nullopt;
  }
  if (cfg->output.shm.empty()) { spdlog::error("output.shm 为空"); return std::nullopt; }
  return *cfg;
}

// 一个输出:shm 段(保活 mmap)+ sink(持段 base 的 typed 指针)。
struct Output {
  mdreplay::ShmSegment            seg;
  std::unique_ptr<mdreplay::Sink> sink;
  mdreplay::Kind                  kind;
};

[[nodiscard]] Result<Output> open_output(const Config& cfg) {
  if (cfg.output.format == "book") {
    auto seg = mdreplay::ShmSegment::open(cfg.output.shm, sizeof(v2::Board), v2::SegKind::Board,
                                          sizeof(v2::BoardSlot), gconf::sym::N_GLOBAL_SYMBOL_IDS,
                                          v2::kBoardSchemaHash, cfg.output.create);
    if (!seg) return std::unexpected(seg.error());
    auto* board = reinterpret_cast<v2::Board*>(seg->base());
    return Output{std::move(*seg), std::make_unique<mdreplay::BookSink>(board), mdreplay::Kind::Book};
  }
  auto seg = mdreplay::ShmSegment::open(cfg.output.shm, sizeof(v2::TradeRing), v2::SegKind::BcastRing,
                                        sizeof(v2::TradeEntry), v2::TRADE_RING_CAP,
                                        v2::kTradeSchemaHash, cfg.output.create);
  if (!seg) return std::unexpected(seg.error());
  auto* ring = reinterpret_cast<v2::TradeRing*>(seg->base());
  return Output{std::move(*seg), std::make_unique<mdreplay::TradeSink>(ring), mdreplay::Kind::Trade};
}

[[nodiscard]] Result<std::vector<std::unique_ptr<mdreplay::Source>>>
load_sources(const Config& cfg, mdreplay::Kind kind, std::size_t& skipped) {
  std::vector<std::unique_ptr<mdreplay::Source>> sources;
  for (const auto& fi : mdreplay::discover(cfg.dir, kind)) {
    auto s = mdreplay::load_csv_source(fi.path, fi.kind, skipped);
    if (!s) return std::unexpected(s.error());
    sources.push_back(std::move(*s));
  }
  return sources;
}

void replay(const Config& cfg, std::vector<std::unique_ptr<mdreplay::Source>> sources,
            mdreplay::Sink& sink, std::size_t skipped) {
  mdreplay::Merger   merger(std::move(sources), cfg.start_ns, cfg.end_ns);
  mdreplay::Clock    clock(cfg.realtime);
  mdreplay::Reporter reporter(cfg.progress_sec);
  reporter.add_skipped(skipped);

  while (const auto rec = merger.next()) {
    clock.pace_to(rec->ts_ns);
    (void)sink.write(*rec);
    reporter.on_event(*rec);
  }
  reporter.finish();
}

}  // namespace

int main(int argc, char** argv) {
  const auto cfg = load_with_overrides(argc, argv);
  if (!cfg) return 1;
  spdlog::set_level(spdlog::level::from_str(cfg->log_level));

  auto out = open_output(*cfg);
  if (!out) {
    spdlog::error("open output '{}' failed: {}", cfg->output.shm, mdreplay::to_string(out.error()));
    return 1;
  }
  spdlog::info("attach {:<5} {} ({})", cfg->output.format, cfg->output.shm,
               cfg->output.create ? "created" : "attached");

  std::size_t skipped = 0;
  auto        sources = load_sources(*cfg, out->kind, skipped);
  if (!sources) {
    spdlog::error("load input failed: {}", mdreplay::to_string(sources.error()));
    return 1;
  }
  if (sources->empty()) {
    spdlog::error("no *.{}.csv under '{}'", cfg->output.format, cfg->dir);
    return 1;
  }
  spdlog::info("datas: {} sources, realtime={}, window=[{}..{}]", sources->size(), cfg->realtime,
               cfg->start_ns, cfg->end_ns);

  replay(*cfg, std::move(*sources), *out->sink, skipped);
  return 0;
}
