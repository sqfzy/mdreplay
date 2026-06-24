#pragma once
// discover.hpp — 扫输入目录,挑指定 kind + 格式的文件(单入单出:一次一种)。
// Kind::Book + "csv" → *.book.csv;Kind::Trade + "json" → *.trade.json。
// 按路径排序 → 固定源序(确定性归并的基础)。

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace mdreplay
