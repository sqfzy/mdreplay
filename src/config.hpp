#pragma once
// config.hpp — 解析 config.toml → Config + 校验。toml++ 驱动;datetime 窗口转 epoch ns。
//
// 校验在启动期一次做完(realtime∈[0,1]、output.format∈{book,trade}、outputs 非空),
// 任一不符 → ConfigInvalid 上抛 main 转非零退出(不带病进回放)。

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "error.hpp"
#include "merge.hpp"  // kNoStart / kNoEnd

namespace mdreplay {

struct OutputCfg {
  std::string format;  // "book" | "trade"
  std::string shm;     // 段名
  bool        create;  // 建段 or attach
};

struct Config {
  std::string  input_format{"csv"};
  std::string  dir;
  double       realtime{1.0};
  std::int64_t start_ns{kNoStart};
  std::int64_t end_ns{kNoEnd};
  OutputCfg    output;  // 单入单出:一次只回放一种(book 或 trade)
  std::string  log_level{"info"};
  int          progress_sec{5};
};

// "YYYY-MM-DD HH:MM:SS"(UTC)→ epoch ns;空串 → sentinel(无界)。解析失败 → nullopt。
[[nodiscard]] inline std::optional<std::int64_t> parse_datetime_ns(const std::string& s,
                                                                   std::int64_t empty_sentinel) {
  if (s.empty()) return empty_sentinel;
  std::tm tm{};
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                  &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return std::nullopt;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  const std::time_t secs = timegm(&tm);  // UTC,不受本地时区影响
  if (secs == static_cast<std::time_t>(-1)) return std::nullopt;
  return static_cast<std::int64_t>(secs) * 1'000'000'000LL;
}

// 从已解析的 table 提取 + 校验 → Config(纯,可单测)。
[[nodiscard]] inline Result<Config> parse_config(const toml::table& tbl) {
  Config cfg;
  cfg.input_format = tbl["input"]["format"].value_or<std::string>("csv");
  cfg.dir          = tbl["input"]["dir"].value_or<std::string>("");
  cfg.realtime     = tbl["replay"]["realtime"].value_or(1.0);
  cfg.log_level    = tbl["log"]["level"].value_or<std::string>("info");
  cfg.progress_sec = tbl["log"]["progress_sec"].value_or(5);

  const auto start = parse_datetime_ns(tbl["replay"]["start"].value_or<std::string>(""), kNoStart);
  const auto end   = parse_datetime_ns(tbl["replay"]["end"].value_or<std::string>(""), kNoEnd);
  if (!start || !end) return std::unexpected(Error::ConfigInvalid);
  cfg.start_ns = *start;
  cfg.end_ns   = *end;

  if (const auto* t = tbl["output"].as_table()) {
    cfg.output = OutputCfg{
        (*t)["format"].value_or<std::string>(""),
        (*t)["shm"].value_or<std::string>(""),
        (*t)["create"].value_or(true),
    };
  }

  // 校验
  if (cfg.realtime < 0.0 || cfg.realtime > 1.0) return std::unexpected(Error::ConfigInvalid);
  if (cfg.start_ns > cfg.end_ns) return std::unexpected(Error::ConfigInvalid);
  if ((cfg.output.format != "book" && cfg.output.format != "trade") || cfg.output.shm.empty())
    return std::unexpected(Error::ConfigInvalid);

  return cfg;
}

// 读文件 → 解析 → 校验。文件/语法错误 → ConfigParse。
[[nodiscard]] inline Result<Config> load_config(const std::string& path) {
  try {
    return parse_config(toml::parse_file(path));
  } catch (const toml::parse_error&) {
    return std::unexpected(Error::ConfigParse);
  }
}

}  // namespace mdreplay
