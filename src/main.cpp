// main.cpp — mdreplay CLI:单入单出记录回放(任意格式 → shm / csv / json)。
//
// 一次只一种(book 或 trade);两者都要就跑两遍(改 --kind 与 --output.*)。
// 编排:解析参数 → 加载配置 → 开一个输出 → 载入该类输入并归并 → 按节奏回放。

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

#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/merge.hpp"
#include "core/record.hpp"
#include "core/report.hpp"
#include "input/csv.hpp"
#include "input/discover.hpp"
#include "input/json.hpp"
#include "input/source.hpp"
#include "output/csv.hpp"
#include "output/json.hpp"
#include "output/shm.hpp"
#include "output/sink.hpp"

namespace v2 = gconf::shm::v2;
using mdreplay::Config;
using mdreplay::Kind;
using mdreplay::Result;

namespace {

void print_usage() {
  std::puts(
      "mdreplay — 单入单出记录回放(任意格式 → shm/csv/json)。选项覆盖 config.toml 同名项。\n"
      "一次只回放一种(book 或 trade);两者都要就跑两遍(改 --kind 与 --output.*)。\n"
      "  --config <path>                 配置文件(默认 config.toml)\n"
      "  --format <csv|json>             输入文件格式 (input.format)\n"
      "  --dir <dir>                     输入目录 (input.dir)\n"
      "  --kind <book|trade>             记录类型 (input.kind)\n"
      "  --realtime <0~1>                节奏:1=原速,0.5=2×,0=尽快可复现\n"
      "  --start \"YYYY-MM-DD HH:MM:SS\"    时间窗起(UTC,留空=最早)\n"
      "  --end   \"YYYY-MM-DD HH:MM:SS\"    时间窗止(UTC,闭区间)\n"
      "  --output.format <shm|csv|json>  去向 (output.format)\n"
      "  --output.shm <name>             shm 段名(format=shm)\n"
      "  --output.path <file>            输出文件(format=csv|json)\n"
      "  --output.create <true|false>    建段(true)/attach(false)(format=shm)\n"
      "  --log-level <trace|debug|info|warn|error>\n"
      "  --progress-sec <n>              进度日志间隔秒\n"
      "  --help                          显示本帮助");
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view s) {
  if (s == "true" || s == "1") return true;
  if (s == "false" || s == "0") return false;
  return std::nullopt;
}

// 严格数值解析:必须整串消费,否则 nullopt(避免 atof/atoi 把坏值静默归 0)。
[[nodiscard]] std::optional<double> parse_double(const std::string& s) {
  char*        end = nullptr;
  const double v   = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return std::nullopt;
  return v;
}
[[nodiscard]] std::optional<int> parse_int(const std::string& s) {
  char*      end = nullptr;
  const long v   = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0') return std::nullopt;
  return static_cast<int>(v);
}

// log-level 串 → spdlog 枚举。兼容 warn/warning、error/err;未知 → nullopt(由调用方报错退出)。
[[nodiscard]] std::optional<spdlog::level::level_enum> parse_log_level(std::string_view s) {
  using L = spdlog::level::level_enum;
  if (s == "trace") return L::trace;
  if (s == "debug") return L::debug;
  if (s == "info") return L::info;
  if (s == "warn" || s == "warning") return L::warn;
  if (s == "error" || s == "err") return L::err;
  if (s == "critical") return L::critical;
  if (s == "off") return L::off;
  return std::nullopt;
}

// 应用 "--output.<key> <val>" 覆盖到 cfg.output(镜像 [output] 表路径)。
[[nodiscard]] bool apply_output(Config& cfg, std::string_view key, const std::string& val) {
  if (key == "format") cfg.output.format = val;
  else if (key == "shm") cfg.output.shm = val;
  else if (key == "path") cfg.output.path = val;
  else if (key == "create") {
    const auto b = parse_bool(val);
    if (!b) { spdlog::error("--output.create 需 true/false"); return false; }
    cfg.output.create = *b;
  } else {
    spdlog::error("--output.{} 未知键(应为 format/shm/path/create)", key);
    return false;
  }
  return true;
}

[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  std::string                config_path = "config.toml";
  std::optional<std::string> ov_format, ov_dir, ov_kind, ov_start, ov_end, ov_log;
  std::optional<double>      ov_realtime;
  std::optional<int>         ov_progress;
  std::vector<std::pair<std::string, std::string>> out_ovr;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
    if (a == "--help") { print_usage(); std::exit(0); }
    else if (a == "--config") config_path = next();
    else if (a == "--format") ov_format = next();
    else if (a == "--dir") ov_dir = next();
    else if (a == "--kind") ov_kind = next();
    else if (a == "--realtime") {
      if (!(ov_realtime = parse_double(next()))) { spdlog::error("--realtime 需数字"); return std::nullopt; }
    }
    else if (a == "--start") ov_start = next();
    else if (a == "--end") ov_end = next();
    else if (a == "--log-level") ov_log = next();
    else if (a == "--progress-sec") {
      if (!(ov_progress = parse_int(next()))) { spdlog::error("--progress-sec 需整数"); return std::nullopt; }
    }
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
  if (ov_kind) cfg->input_kind = *ov_kind;
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

  // 覆盖后复校验(CLI 可能注入非法值)
  if (cfg->realtime < 0.0 || cfg->realtime > 1.0) {
    spdlog::error("realtime 须 ∈ [0,1](得到 {})", cfg->realtime);
    return std::nullopt;
  }
  if (cfg->start_ns > cfg->end_ns) { spdlog::error("start > end"); return std::nullopt; }
  if (cfg->input_format != "csv" && cfg->input_format != "json") {
    spdlog::error("input.format 须 csv/json(得到 '{}')", cfg->input_format);
    return std::nullopt;
  }
  if (cfg->input_kind != "book" && cfg->input_kind != "trade") {
    spdlog::error("input.kind 须 book/trade(得到 '{}')", cfg->input_kind);
    return std::nullopt;
  }
  const auto& o = cfg->output;
  if (o.format == "shm" ? o.shm.empty() : (o.format == "csv" || o.format == "json") ? o.path.empty()
                                                                                    : true) {
    spdlog::error("output 非法:format='{}' shm='{}' path='{}'", o.format, o.shm, o.path);
    return std::nullopt;
  }
  return *cfg;
}

// 一个输出:shm 段(format=shm 时保活 mmap;否则空)+ sink。
struct Output {
  mdreplay::ShmSegment            seg;
  std::unique_ptr<mdreplay::Sink> sink;
};

[[nodiscard]] Result<Output> open_output(const Config& cfg, Kind kind) {
  const auto& o = cfg.output;
  if (o.format == "shm") {
    if (kind == Kind::Book) {
      auto seg = mdreplay::ShmSegment::open(o.shm, sizeof(v2::Board), v2::SegKind::Board,
                                            sizeof(v2::BoardSlot), gconf::sym::N_GLOBAL_SYMBOL_IDS,
                                            v2::kBoardSchemaHash, o.create);
      if (!seg) return std::unexpected(seg.error());
      auto* board = reinterpret_cast<v2::Board*>(seg->base());
      return Output{std::move(*seg), std::make_unique<mdreplay::BookSink>(board)};
    }
    auto seg = mdreplay::ShmSegment::open(o.shm, sizeof(v2::TradeRing), v2::SegKind::BcastRing,
                                          sizeof(v2::TradeEntry), v2::TRADE_RING_CAP,
                                          v2::kTradeSchemaHash, o.create);
    if (!seg) return std::unexpected(seg.error());
    auto* ring = reinterpret_cast<v2::TradeRing*>(seg->base());
    return Output{std::move(*seg), std::make_unique<mdreplay::TradeSink>(ring)};
  }
  if (o.format == "csv") {
    auto s = mdreplay::CsvSink::open(o.path, kind);
    if (!s) return std::unexpected(s.error());
    return Output{{}, std::move(*s)};
  }
  auto s = mdreplay::JsonSink::open(o.path, kind);  // json
  if (!s) return std::unexpected(s.error());
  return Output{{}, std::move(*s)};
}

[[nodiscard]] std::vector<std::unique_ptr<mdreplay::Source>>
load_sources(const Config& cfg, Kind kind, std::size_t& skipped) {
  std::vector<std::unique_ptr<mdreplay::Source>> sources;
  for (const auto& fi : mdreplay::discover(cfg.dir, kind, cfg.input_format)) {
    auto s = (cfg.input_format == "json") ? mdreplay::load_json_source(fi.path, fi.kind, skipped)
                                          : mdreplay::load_csv_source(fi.path, fi.kind, skipped);
    if (!s) {  // 单文件错误(打不开/缺列)→ 命名跳过、不中断其余文件
      spdlog::warn("skip input '{}': {}", fi.path, mdreplay::to_string(s.error()));
      continue;
    }
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
  auto cfg = load_with_overrides(argc, argv);
  if (!cfg) return 1;

  const auto lvl = parse_log_level(cfg->log_level);
  if (!lvl) {
    spdlog::error("log-level 非法 '{}'(应为 trace/debug/info/warn/error/critical/off)",
                  cfg->log_level);
    return 1;
  }
  spdlog::set_level(*lvl);

  const Kind kind = (cfg->input_kind == "book") ? Kind::Book : Kind::Trade;

  // 文件输出无节奏意义:忽略 realtime,按尽快回放(避免原速写文件白等)。
  if (cfg->output.format != "shm" && cfg->realtime > 0.0) {
    spdlog::warn("文件输出忽略 realtime={},按尽快回放", cfg->realtime);
    cfg->realtime = 0.0;
  }

  auto out = open_output(*cfg, kind);
  if (!out) {
    spdlog::error("open output failed: {}", mdreplay::to_string(out.error()));
    return 1;
  }
  if (cfg->output.format == "shm")
    spdlog::info("output: shm → {} ({})", cfg->output.shm,
                 cfg->output.create ? "created" : "attached");
  else
    spdlog::info("output: {} → {}", cfg->output.format, cfg->output.path);

  std::size_t skipped = 0;
  auto        sources = load_sources(*cfg, kind, skipped);
  if (sources.empty()) {
    spdlog::error("no *.{}.{} under '{}'", cfg->input_kind, cfg->input_format, cfg->dir);
    return 1;
  }
  spdlog::info("input: {} {} sources, realtime={}, window=[{}..{}]", sources.size(),
               cfg->input_format, cfg->realtime, cfg->start_ns, cfg->end_ns);

  replay(*cfg, std::move(sources), *out->sink, skipped);
  return 0;
}
