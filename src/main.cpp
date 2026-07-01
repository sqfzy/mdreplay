// main.cpp — mdreplay CLI:多路同步回放(任意格式 → shm)。
//
// 每路 (input → output) 由 config 的一个 [[replays]] 定义,N 路共享顶层 realtime/init_ts 一条全局时钟:
// 全局归并成一条有序流,按 unit 路由到各自的 Sink。单路 = 写 1 个 [[replays]],行为与旧版逐字节一致。
// 编排:解析参数 → 加载配置 → 逐路开输出 + 载入源(拼全局列表 + 记 unit) → 全局归并按节奏路由回放。

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <gconf/shm/v2/booktick_board.h>
#include <gconf/shm/v2/trade.h>
#include <gconf/symbol/symbol_idx.h>

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
#include "output/shm.hpp"
#include "output/sink.hpp"

namespace v2 = gconf::shm::v2;
using mdreplay::Config;
using mdreplay::Kind;
using mdreplay::Result;

namespace {

void print_usage() {
  std::puts(
      "mdreplay — 多路同步回放(任意格式 → shm)。每路 (input→output) 由 config 的 [[replays]] 定义,\n"
      "N 路共享顶层 realtime/init_ts 一条时钟。per-replay 字段(input/output/start/end)只在 config 配置;\n"
      "下列为全局 flag,覆盖 config 同名项。\n"
      "  --config <path>                 配置文件(默认 config.toml)\n"
      "  --realtime <0~1>                全局节奏:1=原速,0.5=2×,0=尽快可复现(顶层 realtime)\n"
      "  --init_ts <ts>                  手动数据原点(UTC datetime 或 epoch ns;对齐到回放开始的墙钟 now);\n"
      "                                  缺省=自动锚首个被回放事件(= 窗口内最早)\n"
      "  --log-level <trace|debug|info|warn|error>\n"
      "  --progress-sec <n>              进度日志间隔秒\n"
      "  --help                          显示本帮助");
}

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

// CLI 只覆盖顶层全局项(per-replay 字段一律 config 文件)。
struct CliOverrides {
  std::string                config_path = "config.toml";
  std::optional<std::string> log, init_ts;
  std::optional<double>      realtime;
  std::optional<int>         progress;
};

// 扫 argv → 全局 flag 覆盖值。--help 直接退出;未知参数 / 坏数值 → nullopt(已报错)。
[[nodiscard]] std::optional<CliOverrides> parse_cli(int argc, char** argv) {
  CliOverrides ov;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
    if (a == "--help") { print_usage(); std::exit(0); }
    else if (a == "--config") ov.config_path = next();
    else if (a == "--realtime") {
      if (!(ov.realtime = parse_double(next()))) { spdlog::error("--realtime 需数字"); return std::nullopt; }
    }
    else if (a == "--init_ts") ov.init_ts = next();
    else if (a == "--log-level") ov.log = next();
    else if (a == "--progress-sec") {
      if (!(ov.progress = parse_int(next()))) { spdlog::error("--progress-sec 需整数"); return std::nullopt; }
    }
    else { spdlog::error("unknown arg: {} (--help 查看用法;per-replay 字段请在 config 配置)", a); return std::nullopt; }
  }
  return ov;
}

// 把 CLI 覆盖叠到 config 上 + 覆盖后复校验(CLI 可能注入非法值)。失败 → false(已报错)。
[[nodiscard]] bool apply_overrides(Config& cfg, const CliOverrides& ov) {
  if (ov.realtime) cfg.realtime = *ov.realtime;
  if (ov.log) cfg.log_level = *ov.log;
  if (ov.progress) cfg.progress_sec = *ov.progress;
  if (ov.init_ts) {  // 手动数据原点(datetime 或 epoch ns);空串=清除(回自动)
    if (ov.init_ts->empty()) cfg.init_ts = std::nullopt;
    else if (const auto v = mdreplay::parse_ts_value(*ov.init_ts)) cfg.init_ts = *v;
    else { spdlog::error("--init_ts 非法(需 UTC datetime \"YYYY-MM-DD HH:MM:SS\" 或 epoch ns 整数)"); return false; }
  }
  if (cfg.realtime < 0.0 || cfg.realtime > 1.0) {  // replays 结构在 parse_config 已校验
    spdlog::error("realtime 须 ∈ [0,1](得到 {})", cfg.realtime);
    return false;
  }
  return true;
}

// 加载配置 + 叠 CLI 全局覆盖:扫参数 → load → apply → 复校验。
[[nodiscard]] std::optional<Config> load_with_overrides(int argc, char** argv) {
  const auto ov = parse_cli(argc, argv);
  if (!ov) return std::nullopt;
  auto cfg = mdreplay::load_config(ov->config_path);
  if (!cfg) {
    spdlog::error("load config '{}' failed: {}", ov->config_path, mdreplay::to_string(cfg.error()));
    return std::nullopt;
  }
  if (!apply_overrides(*cfg, *ov)) return std::nullopt;
  return *cfg;
}

// 一个输出:shm 段(保活 mmap)+ sink。vector<Output> 在回放期保活所有段。
struct Output {
  mdreplay::ShmSegment            seg;
  std::unique_ptr<mdreplay::Sink> sink;
};

// 输出恒为 shm:book depth==1 → BookTickBoard(单档 BBO,gconf v1.2.2 生产段,契约不变);
// book depth>1 → DepthBoard(mdreplay 本地多档段,写全档);trade → TradeRing(临时旧段)。
[[nodiscard]] Result<Output> open_output(const mdreplay::OutputCfg& o, Kind kind, std::size_t depth) {
  if (kind == Kind::Book && depth == 1) {  // BBO:走生产 BookTickBoard,逐字节不变
    auto seg = mdreplay::ShmSegment::open(o.path, sizeof(v2::BookTickBoard), v2::SegKind::Board,
                                          sizeof(v2::BookTickBoardSlot), gconf::sym::N_SYMS,
                                          v2::kBoardSchemaHash, o.create);
    if (!seg) return std::unexpected(seg.error());
    auto* board = reinterpret_cast<v2::BookTickBoard*>(seg->base());
    return Output{std::move(*seg), std::make_unique<mdreplay::BookSink>(board)};
  }
  if (kind == Kind::Book) {  // depth>1:走本地多档 DepthBoard,写全 depth 档
    auto seg = mdreplay::ShmSegment::open(o.path, sizeof(mdreplay::DepthBoard), v2::SegKind::Board,
                                          sizeof(mdreplay::DepthBoardSlot), gconf::sym::N_SYMS,
                                          mdreplay::kDepthBoardSchemaHash, o.create);
    if (!seg) return std::unexpected(seg.error());
    auto* board = reinterpret_cast<mdreplay::DepthBoard*>(seg->base());
    return Output{std::move(*seg), std::make_unique<mdreplay::DepthSink>(board)};
  }
  auto seg = mdreplay::ShmSegment::open(o.path, sizeof(v2::TradeRing), v2::SegKind::BcastRing,
                                        sizeof(v2::TradeEntry), v2::TRADE_RING_CAP,
                                        v2::kTradeSchemaHash, o.create);
  if (!seg) return std::unexpected(seg.error());
  auto* ring = reinterpret_cast<v2::TradeRing*>(seg->base());
  return Output{std::move(*seg), std::make_unique<mdreplay::TradeSink>(ring)};
}

// 探测输入档数(决定 book 输出段:1=BookTickBoard / >1=DepthBoard)。取首个非空源首条记录档数;
// 一路内各源档数必须一致——不一致即 nullopt(段只有一种档数,WARN-截断会静默丢档,违「绝不截断」)。无记录 → 1。
[[nodiscard]] std::optional<std::size_t> detect_depth(
    const std::vector<std::unique_ptr<mdreplay::Source>>& sources) {
  std::optional<std::size_t> depth;
  for (const auto& s : sources) {
    const mdreplay::Record* r = s->peek();
    if (!r) continue;
    if (!depth) depth = r->depth;
    else if (r->depth != *depth) {
      spdlog::error("一路内各源档数不一致(有 {} 档也有 {} 档);单段只容一种档数,拒绝该路"
                    "(请把不同档数的输入分到不同 [[replays]])", *depth, r->depth);
      return std::nullopt;
    }
  }
  return depth.value_or(1);
}

// 载入一路输入:discover 该路 dir/kind/format 下的文件,逐个建流式 Source。单文件错误命名跳过、不中断。
[[nodiscard]] std::vector<std::unique_ptr<mdreplay::Source>>
load_sources(const mdreplay::InputCfg& in, Kind kind, mdreplay::SkipStats& skips) {
  std::vector<std::unique_ptr<mdreplay::Source>> sources;
  for (const auto& fi : mdreplay::discover(in.dir, kind, in.format)) {
    auto s = (in.format == "json") ? mdreplay::load_json_source(fi.path, fi.kind, skips)
                                    : mdreplay::load_csv_source(fi.path, fi.kind, skips);
    if (!s) {  // 单文件错误(打不开/缺列)→ 命名跳过、不中断其余文件
      spdlog::warn("skip input '{}': {}", fi.path, mdreplay::to_string(s.error()));
      continue;
    }
    sources.push_back(std::move(*s));
  }
  return sources;
}

// init_ts + realtime>0:首个**被回放**事件(已过 per-replay 窗口过滤,故是真正播出的第一条)的播出延迟。
// 负值 = init_ts 落在数据之后 → 早于 init_ts 的事件偏移钳 0、立即 burst(WARN);正常为正(INFO)。
// init_ts 远离数据(如手滑写 0 → 数十年)时,这条让异常一眼可见。
void log_init_delay(const Config& cfg, std::int64_t first_ts) {
  if (!cfg.init_ts || cfg.realtime <= 0.0) return;
  const double delay_s = static_cast<double>(first_ts - *cfg.init_ts) * cfg.realtime / 1e9;
  if (delay_s < 0.0)
    spdlog::warn("init_ts={}ns 落在首个被回放事件(ts={}ns)之后 → 早于 init_ts 的事件将立即 burst"
                 "(delay={:.1f}s);多半 init_ts 配错", *cfg.init_ts, first_ts, delay_s);
  else
    spdlog::info("init_ts={}ns → 首个被回放事件(ts={}ns)约 {:.1f}s 后播出", *cfg.init_ts, first_ts, delay_s);
}

// 全局归并 + 按 unit 路由回放。
void replay(const Config& cfg, std::vector<std::unique_ptr<mdreplay::Source>> sources,
            std::vector<std::size_t> src_unit,
            std::vector<std::pair<std::int64_t, std::int64_t>> unit_window,
            std::vector<Output>& opens, const mdreplay::SkipStats& skips) {
  mdreplay::Merger   merger(std::move(sources), std::move(src_unit), std::move(unit_window));
  mdreplay::Clock    clock(cfg.realtime, cfg.init_ts);  // init_ts 缺省=自动锚首个被回放事件
  mdreplay::Reporter reporter(cfg.progress_sec, skips);  // skip 惰性累加,reporter 实时读 total
  std::uint64_t      write_fails = 0;                    // sink 写失败计数(不丢信号,收尾 WARN)
  bool               first = true;
  while (!mdreplay::stop_requested()) {
    const auto tagged = merger.next();
    if (!tagged) break;
    const mdreplay::Record& rec = tagged->rec;
    if (first) { first = false; log_init_delay(cfg, rec.ts_ns); }  // 真正首条(已过窗口)→ 延迟准确
    clock.pace_to(rec.ts_ns, mdreplay::g_stop_requested);
    if (mdreplay::stop_requested()) break;  // pacing 被信号打断 → 不发这条半路事件,停在干净边界
    if (!opens[tagged->unit].sink->write(rec)) ++write_fails;  // 按 unit 路由;写失败计数、不静默吞
    reporter.on_event(rec);
  }
  reporter.finish();
  if (write_fails > 0)  // 正常回放恒 0;>0 = sink 写入有失败(段满/契约异常),不该静默
    spdlog::warn("sink 写入失败 {} 条(未落段);请检查输出段状态", write_fails);
}

// 打印一路装配结果(段类型 + 窗口边界,sentinel 渲染成「最早/最晚」)。
void log_replay_opened(std::size_t unit, const mdreplay::InputCfg& in, std::size_t n_sources,
                       Kind kind, std::size_t depth, const mdreplay::ReplayCfg& rc) {
  const char* seg = (kind != Kind::Book) ? "TradeRing" : (depth == 1 ? "BookTickBoard(BBO)" : "DepthBoard(多档)");
  const std::string lo = (rc.start_ns == mdreplay::kNoStart) ? "最早" : std::to_string(rc.start_ns);
  const std::string hi = (rc.end_ns == mdreplay::kNoEnd) ? "最晚" : std::to_string(rc.end_ns);
  spdlog::info("replay#{}: input {} {} ({} 源, {} 档) → {} [{}] ({}), window=[{}..{}]", unit, in.format,
               in.kind, n_sources, depth, rc.output.path, seg, rc.output.create ? "created" : "attached", lo, hi);
}

// 载入一路 + 开输出 + 把其源拼进全局列表(记 unit)。返回 false = 该路无源/开输出失败(已报错)。
[[nodiscard]] bool load_one_replay(const mdreplay::ReplayCfg& rc, std::size_t unit,
                                   mdreplay::SkipStats& skips,
                                   std::vector<std::unique_ptr<mdreplay::Source>>& global_sources,
                                   std::vector<std::size_t>& src_unit, std::vector<Output>& opens) {
  const Kind kind = (rc.input.kind == "book") ? Kind::Book : Kind::Trade;

  mdreplay::InputCfg in = rc.input;
  if (in.format == "auto") {  // 按本路目录自动识别 csv/json
    const auto fmt = mdreplay::detect_input_format(in.dir, kind);
    if (!fmt) return false;   // detect 已自描述报错
    in.format = *fmt;
    spdlog::info("replay#{} input.format=auto → 识别为 {}", unit, in.format);
  }

  auto sources = load_sources(in, kind, skips);
  if (sources.empty()) {
    spdlog::error("replay#{}: no *.{}.{} under '{}'", unit, in.kind, in.format, in.dir);
    return false;
  }
  std::size_t depth = 1;
  if (kind == Kind::Book) {
    const auto d = detect_depth(sources);  // 一路内档数不一致 → 已报错,拒该路
    if (!d) return false;
    depth = *d;
  }

  auto out = open_output(rc.output, kind, depth);
  if (!out) {
    spdlog::error("replay#{} open output '{}' failed: {}", unit, rc.output.path,
                  mdreplay::to_string(out.error()));
    return false;
  }
  log_replay_opened(unit, in, sources.size(), kind, depth, rc);

  for (auto& s : sources) { global_sources.push_back(std::move(s)); src_unit.push_back(unit); }
  opens.push_back(std::move(*out));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  auto cfg = load_with_overrides(argc, argv);
  if (!cfg) return 1;

  mdreplay::install_signal_handlers();  // SIGINT/SIGTERM → 优雅停止(收尾后退出),不被硬杀

  const auto lvl = parse_log_level(cfg->log_level);
  if (!lvl) {
    spdlog::error("log-level 非法 '{}'(应为 trace/debug/info/warn/error/critical/off)", cfg->log_level);
    return 1;
  }
  spdlog::set_level(*lvl);

  if (cfg->init_ts)  // 手动数据原点:仅 realtime>0 生效(realtime=0 尽快、无节奏)
    spdlog::info("init_ts={}ns(手动数据原点;realtime=0 时无效)", *cfg->init_ts);

  // 逐路:载入源(拼全局列表 + 记 unit)+ 开输出。unit-major 拼接 → 源下标天然编码 (unit, 源内序)。
  mdreplay::SkipStats                            skips;
  std::vector<std::unique_ptr<mdreplay::Source>> global_sources;
  std::vector<std::size_t>                       src_unit;
  std::vector<Output>                            opens;
  std::vector<std::pair<std::int64_t, std::int64_t>> unit_window;
  for (std::size_t u = 0; u < cfg->replays.size(); ++u) {
    const auto& rc = cfg->replays[u];
    if (!load_one_replay(rc, u, skips, global_sources, src_unit, opens)) return 1;
    unit_window.emplace_back(rc.start_ns, rc.end_ns);
  }
  spdlog::info("loaded {} replays, {} sources total, realtime={}", cfg->replays.size(),
               global_sources.size(), cfg->realtime);

  replay(*cfg, std::move(global_sources), std::move(src_unit), std::move(unit_window), opens, skips);
  for (auto& o : opens) o.sink->on_finish();  // 各路去向相关收尾(trade 环绕圈告警等)
  if (mdreplay::stop_requested())
    spdlog::warn("收到中断信号,已优雅停止(产出截至中断点有效,下方为截至此刻的汇总)");
  skips.log_summary();  // 流式:回放后一次性交代分原因明细 + 未知符号 WARN
  return 0;
}
