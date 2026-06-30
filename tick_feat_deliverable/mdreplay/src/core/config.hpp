#pragma once
// config.hpp — 解析 config.toml → Config + 校验。toml++ 驱动;datetime 窗口转 epoch ns。
//
// 多路同步回放:顶层 realtime/init_ts 全局(一条时钟),[[replays]] 数组每元素是一个自包含
// (input + output + 本路时间窗) 单元——input/output 绑成一对,无 index 配对、无等长校验。
// 校验在启动期一次做完(realtime∈[0,1]、各 input.kind∈{book,trade}、各 input.format∈{csv,json,auto}、
// 各 output.path 非空且两两不同、各 start≤end),任一不符 → ConfigInvalid 上抛 main 转非零退出。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include "core/error.hpp"
#include "core/window.hpp"  // kNoStart / kNoEnd

namespace mdreplay {

// 一路输入:格式 + 目录 + 记录类型。
struct InputCfg {
  std::string format{"csv"};  // csv | json | auto
  std::string dir;
  std::string kind{"book"};   // book | trade
};

struct OutputCfg {
  std::string path;          // shm 段名(/开头)
  bool        create{true};  // 建段(O_TRUNC 幂等)or attach
};

// 一路回放单元:input → output,带本路独立时间窗(per-replay)。多路共享顶层 realtime/init_ts 一条时钟。
struct ReplayCfg {
  InputCfg     input;
  OutputCfg    output;
  std::int64_t start_ns{kNoStart};  // 本路时间窗起(epoch ns;留空=最早)
  std::int64_t end_ns{kNoEnd};      // 本路时间窗止(留空=最晚)
};

struct Config {
  double                      realtime{1.0};  // 全局:0~1 播放速率
  std::optional<std::int64_t> init_ts;        // 全局:手动数据原点(epoch ns);缺省=自动锚首个被回放事件
  std::vector<ReplayCfg>      replays;         // [[replays]] —— 至少 1 路
  std::string                 log_level{"info"};
  int                         progress_sec{5};
};

// "YYYY-MM-DD HH:MM:SS"(UTC)→ epoch ns;空串 → sentinel(无界)。解析失败 → nullopt。
[[nodiscard]] inline std::optional<std::int64_t> parse_datetime_ns(const std::string& s,
                                                                   std::int64_t empty_sentinel) {
  if (s.empty()) return empty_sentinel;
  int  y, mo, d, h, mi, se;
  char extra;  // 第 7 个 %c 命中(n==7)= 有尾随字符 → 拒;n<6 = 不完整 → 拒
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d%c", &y, &mo, &d, &h, &mi, &se, &extra) != 6)
    return std::nullopt;

  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon  = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min  = mi;
  tm.tm_sec  = se;
  const std::time_t secs = timegm(&tm);  // UTC,不受本地时区影响
  if (secs == static_cast<std::time_t>(-1)) return std::nullopt;

  // 回写比对:timegm 把越界字段(如 2026-02-30)静默归一化;归一化后与输入不符即判非法。
  std::tm chk{};
  gmtime_r(&secs, &chk);
  if (chk.tm_year != tm.tm_year || chk.tm_mon != tm.tm_mon || chk.tm_mday != tm.tm_mday ||
      chk.tm_hour != tm.tm_hour || chk.tm_min != tm.tm_min || chk.tm_sec != tm.tm_sec)
    return std::nullopt;

  return static_cast<std::int64_t>(secs) * 1'000'000'000LL;
}

// 时刻取值:裸整数 = epoch ns;否则按 UTC datetime("YYYY-MM-DD HH:MM:SS")。空/非法 → nullopt。
[[nodiscard]] inline std::optional<std::int64_t> parse_ts_value(const std::string& s) {
  if (s.empty()) return std::nullopt;
  char*           end = nullptr;
  const long long v   = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() + s.size()) return static_cast<std::int64_t>(v);  // 全消费 → 裸 ns
  return parse_datetime_ns(s, 0);                                        // 否则 datetime(非空,失败→nullopt)
}

// 顶层 `init_ts`(手动数据原点):整数当裸 ns,字符串走 parse_ts_value,空串=不设(自动)。
// 返回 nullopt 表「没配/留空」,error 表「配了但非法」。
[[nodiscard]] inline std::optional<Result<std::int64_t>> parse_init_ts(const toml::table& tbl) {
  const auto n = tbl["init_ts"];
  if (const auto i = n.value<std::int64_t>()) return Result<std::int64_t>{*i};        // 裸 ns
  if (const auto s = n.value<std::string>()) {
    if (s->empty()) return std::nullopt;                                              // 留空=自动
    if (const auto v = parse_ts_value(*s)) return Result<std::int64_t>{*v};
    return Result<std::int64_t>{std::unexpected(Error::ConfigInvalid)};               // 非法
  }
  return std::nullopt;                                                                // 没配=自动
}

// 解析一个 [[replays]] 元素(input/output 子 inline 表 + 本路 start/end)。结构缺失/窗口非法 → ConfigInvalid。
[[nodiscard]] inline Result<ReplayCfg> parse_replay(const toml::table& e) {
  const auto* in_t  = e["input"].as_table();
  const auto* out_t = e["output"].as_table();
  if (!in_t || !out_t) return std::unexpected(Error::ConfigInvalid);  // 每路必须有 input + output

  ReplayCfg r;
  r.input  = InputCfg{(*in_t)["format"].value_or<std::string>("csv"),
                      (*in_t)["dir"].value_or<std::string>(""),
                      (*in_t)["kind"].value_or<std::string>("book")};
  r.output = OutputCfg{(*out_t)["path"].value_or<std::string>(""),
                       (*out_t)["create"].value_or(true)};

  const auto start = parse_datetime_ns(e["start"].value_or<std::string>(""), kNoStart);
  const auto end   = parse_datetime_ns(e["end"].value_or<std::string>(""), kNoEnd);
  if (!start || !end) return std::unexpected(Error::ConfigInvalid);
  r.start_ns = *start;
  r.end_ns   = *end;
  return r;
}

// 从已解析的 table 提取 + 校验 → Config(纯,可单测)。
[[nodiscard]] inline Result<Config> parse_config(const toml::table& tbl) {
  Config cfg;
  cfg.realtime     = tbl["realtime"].value_or(1.0);
  cfg.log_level    = tbl["log"]["level"].value_or<std::string>("info");
  cfg.progress_sec = tbl["log"]["progress_sec"].value_or(5);

  if (auto it = parse_init_ts(tbl)) {  // 配了 init_ts(合法/非法)
    if (!*it) return std::unexpected((*it).error());
    cfg.init_ts = **it;
  }

  // [[replays]]:数组每元素一路。缺失整个数组 / 空数组 → 后面校验拒。
  if (const auto* arr = tbl["replays"].as_array()) {
    for (const auto& node : *arr) {
      const auto* e = node.as_table();
      if (!e) return std::unexpected(Error::ConfigInvalid);  // 元素必须是表
      auto r = parse_replay(*e);
      if (!r) return std::unexpected(r.error());
      cfg.replays.push_back(std::move(*r));
    }
  }

  // 校验(失败即指明是哪项/哪路 —— 段名重复 vs start>end 不能都收敛成同一句无信息的 "config invalid")
  if (cfg.realtime < 0.0 || cfg.realtime > 1.0) {
    spdlog::error("config: realtime={} 不在 [0,1]", cfg.realtime);
    return std::unexpected(Error::ConfigInvalid);
  }
  if (cfg.replays.empty()) {  // 至少 1 路
    spdlog::error("config: 至少需要一个 [[replays]]");
    return std::unexpected(Error::ConfigInvalid);
  }
  std::unordered_set<std::string> seen_paths;
  for (std::size_t i = 0; i < cfg.replays.size(); ++i) {
    const auto& r = cfg.replays[i];
    if (r.input.kind != "book" && r.input.kind != "trade") {
      spdlog::error("config: replay#{} input.kind='{}' 非 book/trade", i, r.input.kind);
      return std::unexpected(Error::ConfigInvalid);
    }
    if (r.input.format != "csv" && r.input.format != "json" && r.input.format != "auto") {
      spdlog::error("config: replay#{} input.format='{}' 非 csv/json/auto", i, r.input.format);
      return std::unexpected(Error::ConfigInvalid);
    }
    if (r.output.path.empty()) {  // 段名必填
      spdlog::error("config: replay#{} output.path 为空(shm 段名必填)", i);
      return std::unexpected(Error::ConfigInvalid);
    }
    if (!seen_paths.insert(r.output.path).second) {  // 段名两两不同,防多路互覆盖同段
      spdlog::error("config: replay#{} output.path='{}' 与前面某路重复(段名须两两不同)", i, r.output.path);
      return std::unexpected(Error::ConfigInvalid);
    }
    if (r.start_ns > r.end_ns) {
      spdlog::error("config: replay#{} start({}) > end({})", i, r.start_ns, r.end_ns);
      return std::unexpected(Error::ConfigInvalid);
    }
  }

  return cfg;
}

// 读文件 → 解析 → 校验。文件/语法错误 → ConfigParse。
[[nodiscard]] inline Result<Config> load_config(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::unexpected(Error::ConfigNotFound);
  try {
    return parse_config(toml::parse_file(path));
  } catch (const toml::parse_error&) {
    return std::unexpected(Error::ConfigParse);
  }
}

}  // namespace mdreplay
