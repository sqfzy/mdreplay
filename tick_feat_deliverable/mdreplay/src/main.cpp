// main.cpp — mdreplay CLI:单入单出记录回放(任意格式 → shm / csv / json)。
//
// 一次只一种(book 或 trade);两者都要就跑两遍(改 --kind 与 --output.*)。
// 编排:解析参数 → 加载配置 → 开一个输出 → 载入该类输入并归并 → 按节奏回放。

#include <chrono>
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
#include "core/signal.hpp"
#include "core/skip.hpp"
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
      "  --format <csv|json|auto>        输入文件格式;auto=扫目录自动识别 (input.format)\n"
      "  --dir <dir>                     输入目录 (input.dir)\n"
      "  --kind <book|trade>             记录类型 (input.kind)\n"
      "  --realtime <0~1>                节奏:1=原速,0.5=2×,0=尽快可复现\n"
      "  --start \"YYYY-MM-DD HH:MM:SS\"    时间窗起(UTC,留空=最早)\n"
      "  --end   \"YYYY-MM-DD HH:MM:SS\"    时间窗止(UTC,闭区间)\n"
      "  --anchor.data_ts   <ts>         时序锚·数据原点(UTC datetime 或 epoch ns)\n"
      "  --anchor.system_ts <ts>         时序锚·墙钟(同 data_ts);两者成对=启用跨进程同钟\n"
      "  --output.format <shm|csv|json>  去向 (output.format)\n"
      "  --output.path <dest>            去向定位:shm 段名(/开头)或输出文件路径\n"
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
  else if (key == "path") cfg.output.path = val;
  else if (key == "create") {
    const auto b = parse_bool(val);
    if (!b) { spdlog::error("--output.create 需 true/false"); return false; }
    cfg.output.create = *b;
  } else {
    spdlog::error("--output.{} 未知键(应为 format/path/create)", key);
    return false;
  }
  return true;
}

// 解析 + 校验 anchor 覆盖:config 既有值上叠 CLI 覆盖。data_ts 与 system_ts 必须成对(完整映射才能
// 定一条时间线);成功写回 cfg.anchor(都不给 → 不同步)。任何非法/只给一半 → false(已报错)。
[[nodiscard]] bool resolve_anchor_override(Config& cfg, const std::optional<std::string>& ov_data,
                                           const std::optional<std::string>& ov_system) {
  constexpr auto kBadValue = "需 UTC datetime \"YYYY-MM-DD HH:MM:SS\" 或 epoch ns 整数";
  std::optional<std::int64_t> data   = cfg.anchor ? std::optional(cfg.anchor->data_ts_ns) : std::nullopt;
  std::optional<std::int64_t> system = cfg.anchor ? std::optional(cfg.anchor->system_ts_ns) : std::nullopt;
  if (ov_data && !(data = mdreplay::parse_anchor_value(*ov_data))) {
    spdlog::error("--anchor.data_ts 非法({})", kBadValue);
    return false;
  }
  if (ov_system && !(system = mdreplay::parse_anchor_value(*ov_system))) {
    spdlog::error("--anchor.system_ts 非法({})", kBadValue);
    return false;
  }
  if (data.has_value() != system.has_value()) {
    spdlog::error("anchor 映射不完整:data_ts 与 system_ts 必须同时给(同步需要完整的 数据时刻↔墙钟 锚点);"
                  "都不给则不同步(默认各锚首事件)");
    return false;
  }
  cfg.anchor = (data && system) ? std::optional(mdreplay::AnchorCfg{*data, *system}) : std::nullopt;
  return true;
}

[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  std::string                config_path = "config.toml";
  std::optional<std::string> ov_format, ov_dir, ov_kind, ov_start, ov_end, ov_log;
  std::optional<std::string> ov_anchor_data, ov_anchor_system;
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
    else if (a == "--anchor.data_ts") ov_anchor_data = next();
    else if (a == "--anchor.system_ts") ov_anchor_system = next();
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

  if (!resolve_anchor_override(*cfg, ov_anchor_data, ov_anchor_system)) return std::nullopt;

  // 覆盖后复校验(CLI 可能注入非法值)
  if (cfg->realtime < 0.0 || cfg->realtime > 1.0) {
    spdlog::error("realtime 须 ∈ [0,1](得到 {})", cfg->realtime);
    return std::nullopt;
  }
  if (cfg->start_ns > cfg->end_ns) { spdlog::error("start > end"); return std::nullopt; }
  if (cfg->input_format != "csv" && cfg->input_format != "json" && cfg->input_format != "auto") {
    spdlog::error("input.format='{}' 不受支持。mdreplay 只解析 csv / json,或 auto 自动识别;"
                  "不支持 parquet 等其它格式。", cfg->input_format);
    return std::nullopt;
  }
  if (cfg->input_kind != "book" && cfg->input_kind != "trade") {
    spdlog::error("input.kind 须 book/trade(得到 '{}')", cfg->input_kind);
    return std::nullopt;
  }
  const auto& o = cfg->output;
  if (o.format != "shm" && o.format != "csv" && o.format != "json") {
    spdlog::error("output.format 须 shm/csv/json(得到 '{}')", o.format);
    return std::nullopt;
  }
  if (o.path.empty()) { spdlog::error("output.path 不能为空"); return std::nullopt; }
  return *cfg;
}

// 一个输出:shm 段(format=shm 时保活 mmap;否则空)+ sink。
struct Output {
  mdreplay::ShmSegment            seg;
  std::unique_ptr<mdreplay::Sink> sink;
};

// depth = 输入自动识别的档数(1 或 5),决定 book→shm 用 Board 还是 DepthBoard、csv 表头档数。
[[nodiscard]] Result<Output> open_output(const Config& cfg, Kind kind, std::size_t depth) {
  const auto& o = cfg.output;
  if (o.format == "shm") {
    if (kind == Kind::Book && depth == 5) {  // 五档 → DepthBoard 段
      auto seg = mdreplay::ShmSegment::open(o.path, sizeof(v2::DepthBoard), v2::SegKind::Board,
                                            sizeof(v2::DepthSlot), gconf::sym::N_GLOBAL_SYMBOL_IDS,
                                            v2::kDepthBoardSchemaHash, o.create);
      if (!seg) return std::unexpected(seg.error());
      auto* board = reinterpret_cast<v2::DepthBoard*>(seg->base());
      return Output{std::move(*seg), std::make_unique<mdreplay::DepthBookSink>(board)};
    }
    if (kind == Kind::Book) {  // BBO → Board 段
      auto seg = mdreplay::ShmSegment::open(o.path, sizeof(v2::Board), v2::SegKind::Board,
                                            sizeof(v2::BoardSlot), gconf::sym::N_GLOBAL_SYMBOL_IDS,
                                            v2::kBoardSchemaHash, o.create);
      if (!seg) return std::unexpected(seg.error());
      auto* board = reinterpret_cast<v2::Board*>(seg->base());
      return Output{std::move(*seg), std::make_unique<mdreplay::BookSink>(board)};
    }
    auto seg = mdreplay::ShmSegment::open(o.path, sizeof(v2::TradeRing), v2::SegKind::BcastRing,
                                          sizeof(v2::TradeEntry), v2::TRADE_RING_CAP,
                                          v2::kTradeSchemaHash, o.create);
    if (!seg) return std::unexpected(seg.error());
    auto* ring = reinterpret_cast<v2::TradeRing*>(seg->base());
    return Output{std::move(*seg), std::make_unique<mdreplay::TradeSink>(ring)};
  }
  if (o.format == "csv") {
    auto s = mdreplay::CsvSink::open(o.path, kind, depth);
    if (!s) return std::unexpected(s.error());
    return Output{{}, std::move(*s)};
  }
  auto s = mdreplay::JsonSink::open(o.path, kind);  // json:逐行按 record.depth 写,无需建表头
  if (!s) return std::unexpected(s.error());
  return Output{{}, std::move(*s)};
}

// 从已载入的源头探测档数(book 自动识别后写进 Record.depth):取首个非空源的首条记录定档;
// 若各源首记录档数不一致 → WARN(输出按首源档数,混档会产出不一致)。无记录 → 默认 1。
[[nodiscard]] std::size_t detect_depth(const std::vector<std::unique_ptr<mdreplay::Source>>& sources) {
  std::size_t depth = 1;
  bool        found = false;
  for (const auto& s : sources) {
    const mdreplay::Record* r = s->peek();
    if (!r) continue;
    if (!found) { depth = r->depth; found = true; }
    else if (r->depth != depth)
      spdlog::warn("源间档数不一致(首源 {} 档,另有 {} 档);输出按 {} 档,混档输出可能不一致",
                   depth, r->depth, depth);
  }
  return depth;
}

[[nodiscard]] std::vector<std::unique_ptr<mdreplay::Source>>
load_sources(const Config& cfg, Kind kind, mdreplay::SkipStats& skips) {
  std::vector<std::unique_ptr<mdreplay::Source>> sources;
  for (const auto& fi : mdreplay::discover(cfg.dir, kind, cfg.input_format)) {
    auto s = (cfg.input_format == "json") ? mdreplay::load_json_source(fi.path, fi.kind, skips)
                                          : mdreplay::load_csv_source(fi.path, fi.kind, skips);
    if (!s) {  // 单文件错误(打不开/缺列)→ 命名跳过、不中断其余文件
      spdlog::warn("skip input '{}': {}", fi.path, mdreplay::to_string(s.error()));
      continue;
    }
    sources.push_back(std::move(*s));
  }
  return sources;
}

// 返回 false = anchor 设置不合理(首事件会 burst),已报错、未发任何事件 → main 转非零退出。
[[nodiscard]] bool replay(const Config& cfg, std::vector<std::unique_ptr<mdreplay::Source>> sources,
                          mdreplay::Sink& sink, const mdreplay::SkipStats& skips) {
  std::optional<mdreplay::Clock::Anchor> anchor;
  if (cfg.anchor) anchor = mdreplay::Clock::Anchor{cfg.anchor->data_ts_ns, cfg.anchor->system_ts_ns};
  mdreplay::Merger   merger(std::move(sources), cfg.start_ns, cfg.end_ns);
  mdreplay::Clock    clock(cfg.realtime, anchor);
  mdreplay::Reporter reporter(cfg.progress_sec, skips);  // skip 惰性累加,reporter 实时读 total
  bool first = true;
  while (!mdreplay::stop_requested()) {
    const auto rec = merger.next();
    if (!rec) break;
    if (first) {  // 强制:最早被回放的事件(最坏情况)不得 burst,否则 anchor 设置不合理 → 拒
      first = false;
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      if (clock.would_burst(rec->ts_ns, now_ns)) {
        spdlog::error("anchor 设置会让首事件 burst:首条事件 ts={}ns,其播出墙钟 {}ns 已早于现在 {}ns。"
                      "病因 = data_ts 落在数据中间(应 ≤ 首条 ts)或 system_ts 不够未来。"
                      "请把 data_ts 设到 ≤ {} 并把 system_ts 取足够未来(如 now+几秒);拒绝启动。",
                      rec->ts_ns, clock.target_system_ns(rec->ts_ns), now_ns, rec->ts_ns);
        return false;
      }
    }
    clock.pace_to(rec->ts_ns, mdreplay::g_stop_requested);
    if (mdreplay::stop_requested()) break;  // pacing 被信号打断 → 不发这条半路事件,停在干净边界
    (void)sink.write(*rec);
    reporter.on_event(*rec);
  }
  reporter.finish();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  auto cfg = load_with_overrides(argc, argv);
  if (!cfg) return 1;

  mdreplay::install_signal_handlers();  // SIGINT/SIGTERM → 优雅停止(收尾后退出),不被硬杀

  const auto lvl = parse_log_level(cfg->log_level);
  if (!lvl) {
    spdlog::error("log-level 非法 '{}'(应为 trace/debug/info/warn/error/critical/off)",
                  cfg->log_level);
    return 1;
  }
  spdlog::set_level(*lvl);

  const Kind kind = (cfg->input_kind == "book") ? Kind::Book : Kind::Trade;

  // format=auto:扫目录自动识别 csv/json(失败时 detect 已自描述报错)。识别后落为具体格式再往下走。
  if (cfg->input_format == "auto") {
    const auto fmt = mdreplay::detect_input_format(cfg->dir, kind);
    if (!fmt) return 1;
    cfg->input_format = *fmt;
    spdlog::info("input.format=auto → 识别为 {}", cfg->input_format);
  }

  // 文件输出无节奏意义:忽略 realtime,按尽快回放(避免原速写文件白等)。
  if (cfg->output.format != "shm" && cfg->realtime > 0.0) {
    spdlog::warn("文件输出忽略 realtime={},按尽快回放", cfg->realtime);
    cfg->realtime = 0.0;
  }

  // 时序锚:仅 realtime>0 生效;realtime=0/文件输出下忽略(本就不限速)。提示 + 过去则告知 burst。
  if (cfg->anchor) {
    if (cfg->realtime <= 0.0) {
      spdlog::info("anchor 已配置,但 realtime=0/文件输出 → 本次忽略(不限速)");
    } else {
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      // 强制:system_ts 必须在未来(否则数据从过去起播 → burst,灌爆 trade 段)。早失败,免载入白跑。
      if (cfg->anchor->system_ts_ns < now_ns) {
        spdlog::error("anchor.system_ts 必须在未来(现在={}ns,得到={}ns)。同步回放需把发令墙钟设到 now 之后"
                      "(如 now+几秒);取过去会让数据从过去起播、burst 灌爆 trade 段 → 拒绝启动。",
                      now_ns, cfg->anchor->system_ts_ns);
        return 1;
      }
      spdlog::info("anchor 启用:data_ts={}ns ↔ system_ts={}ns(多进程填同值 + 同 realtime 即同钟)",
                   cfg->anchor->data_ts_ns, cfg->anchor->system_ts_ns);
    }
  }

  // 先载入(book 自动识别档数,写进 Record.depth),再据探测到的档数开输出(选 Board/DepthBoard、定 csv 表头)。
  mdreplay::SkipStats skips;
  auto                sources = load_sources(*cfg, kind, skips);
  if (sources.empty()) {
    spdlog::error("no *.{}.{} under '{}'", cfg->input_kind, cfg->input_format, cfg->dir);
    return 1;
  }
  const std::size_t depth = (kind == Kind::Book) ? detect_depth(sources) : 1;
  spdlog::info("input: {} {} sources, kind={}, depth={}, realtime={}, window=[{}..{}]", sources.size(),
               cfg->input_format, cfg->input_kind, depth, cfg->realtime, cfg->start_ns, cfg->end_ns);

  auto out = open_output(*cfg, kind, depth);
  if (!out) {
    spdlog::error("open output failed: {}", mdreplay::to_string(out.error()));
    return 1;
  }
  if (cfg->output.format == "shm")
    spdlog::info("output: shm → {} ({}, {} 档)", cfg->output.path,
                 cfg->output.create ? "created" : "attached", depth);
  else
    spdlog::info("output: {} → {} ({} 档)", cfg->output.format, cfg->output.path, depth);

  if (!replay(*cfg, std::move(sources), *out->sink, skips)) return 1;  // anchor 不合理 → 已报错,拒启动
  out->sink->on_finish();          // 去向相关收尾(trade 环绕圈告警等)
  if (mdreplay::stop_requested())  // 收到信号:在干净边界停了,产出截至中断点有效
    spdlog::warn("收到中断信号,已优雅停止(产出截至中断点有效,下方为截至此刻的汇总)");
  // 流式:skip 在回放消费期才累加;回放后一次性交代分原因明细 + 未知符号 WARN。
  skips.log_summary();
  return 0;
}
