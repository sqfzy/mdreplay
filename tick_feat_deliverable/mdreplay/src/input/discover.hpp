#pragma once
// discover.hpp — 扫输入目录,只挑指定 kind 的文件(单入单出:一次一种)。
// Kind::Book → *.book.csv,Kind::Trade → *.trade.csv。按路径排序 → 固定源序(确定性归并的基础)。

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "record.hpp"

namespace mdreplay {

struct InputFile {
  std::string path;
  Kind        kind;
};

[[nodiscard]] inline std::vector<InputFile> discover(const std::string& dir, Kind kind) {
  std::vector<InputFile> out;
  std::error_code        ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;

  const std::string_view suffix = (kind == Kind::Book) ? ".book.csv" : ".trade.csv";
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().filename().string().ends_with(suffix))
      out.push_back({entry.path().string(), kind});
  }
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) { return a.path < b.path; });
  return out;
}

}  // namespace mdreplay
