#pragma once
// discover.hpp — 扫输入目录,挑指定 kind + 格式的文件(每路 [[replays]] 各扫自己的 dir/kind/format)。
// Kind::Book + "csv" → *.book.csv;Kind::Trade + "json" → *.trade.json。
// 按路径排序 → 固定源序(确定性归并的基础)。

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/record.hpp"

namespace mdreplay {

struct InputFile {
  std::string path;
  Kind        kind;
};

[[nodiscard]] inline std::vector<InputFile> discover(const std::string& dir, Kind kind,
                                                     std::string_view ext) {
  std::vector<InputFile> out;
  std::error_code        ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;

  const std::string suffix =
      std::string(".") + (kind == Kind::Book ? "book" : "trade") + "." + std::string(ext);
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().filename().string().ends_with(suffix))
      out.push_back({entry.path().string(), kind});
  }
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) { return a.path < b.path; });
  return out;
}

// format=auto:扫目录里该 kind 的输入自动识别 csv/json。只一种 → 用它;两者并存 → 歧义;都没有 → 报错。
// 失败时**自描述**(说明只支持 csv/json、该怎么办)并返回 nullopt —— 超出能力的请求不让用户干瞪眼。
[[nodiscard]] inline std::optional<std::string> detect_input_format(const std::string& dir, Kind kind) {
  const bool        has_csv  = !discover(dir, kind, "csv").empty();
  const bool        has_json = !discover(dir, kind, "json").empty();
  const std::string k        = (kind == Kind::Book) ? "book" : "trade";
  if (has_csv && has_json) {
    spdlog::error("--format auto:目录 '{}' 下 *.{}.csv 与 *.{}.json 并存,无法二选一;"
                  "请用 --format csv|json 明确指定其一", dir, k, k);
    return std::nullopt;
  }
  if (has_csv) return std::string("csv");
  if (has_json) return std::string("json");
  spdlog::error("--format auto:目录 '{}' 下找不到 *.{}.csv 或 *.{}.json;"
                "mdreplay 只支持 csv / json 两种输入格式(不支持 parquet 等)", dir, k, k);
  return std::nullopt;
}

}  // namespace mdreplay
