#pragma once
// config.hpp — 解析 config.toml → Config + 校验。toml++ 驱动;datetime 窗口转 epoch ns。
//
// 校验在启动期一次做完(realtime∈[0,1]、input.kind∈{book,trade}、output.format∈{shm,csv,json}
// 且对应 shm/path 非空),任一不符 → ConfigInvalid 上抛 main 转非零退出(不带病进回放)。

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

#include <toml++/toml.hpp>

#include "core/error.hpp"
#include "core/window.hpp"  // kNoStart / kNoEnd

namespace mdreplay {

struct OutputCfg {
  std::string format;  // "shm" | "csv" | "json" —— 去向
  std::string shm;     // format=shm 时的段名
  std::string path;    // format=csv|json 时的输出文件
  bool        create{true};  // format=shm 时:建段 or attach
};

struct Config {
  std::string  input_format{"csv"};  // csv | json —— 输入文件格式
  std::string  dir;
  std::string  input_kind{"book"};   // book | trade —— 记录类型(单入单出,一次一种)
  double       realtime{1.0};
  std::int64_t start_ns{kNoStart};
  std::int64_t end_ns{kNoEnd};
  OutputCfg    output;
  std::string  log_level{"info"};
  int          progress_sec{5};
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

// 从已解析的 table 提取 + 校验 → Config(纯,可单测)。
[[nodiscard]] inline Result<Config> parse_config(const toml::table& tbl) {
  Config cfg;
  cfg.input_format = tbl["input"]["format"].value_or<std::string>("csv");
  cfg.dir          = tbl["input"]["dir"].value_or<std::string>("");
  cfg.input_kind   = tbl["input"]["kind"].value_or<std::string>("book");
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
        (*t)["path"].value_or<std::string>(""),
        (*t)["create"].value_or(true),
    };
  }

  // 校验
  if (cfg.realtime < 0.0 || cfg.realtime > 1.0) return std::unexpected(Error::ConfigInvalid);
  if (cfg.start_ns > cfg.end_ns) return std::unexpected(Error::ConfigInvalid);
  if (cfg.input_kind != "book" && cfg.input_kind != "trade")
    return std::unexpected(Error::ConfigInvalid);
  const auto& o = cfg.output;
  if (o.format == "shm") {
    if (o.shm.empty()) return std::unexpected(Error::ConfigInvalid);
  } else if (o.format == "csv" || o.format == "json") {
    if (o.path.empty()) return std::unexpected(Error::ConfigInvalid);
  } else {
    return std::unexpected(Error::ConfigInvalid);  // 未知 output.format
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
