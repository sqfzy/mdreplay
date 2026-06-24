#pragma once
// discover.hpp — 扫输入目录,按文件名后缀分类 book/trade。
// *.book.csv → Kind::Book,*.trade.csv → Kind::Trade。按路径排序 → 固定源序(确定性归并的基础)。

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "record.hpp"

namespace mdreplay {

struct InputFile {
  std::string path;
  Kind        kind;
};

[[nodiscard]] inline std::vector<InputFile> discover(const std::string& dir) {
  std::vector<InputFile> out;
  std::error_code        ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.ends_with(".book.csv"))
      out.push_back({entry.path().string(), Kind::Book});
    else if (name.ends_with(".trade.csv"))
      out.push_back({entry.path().string(), Kind::Trade});
  }
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) { return a.path < b.path; });
  return out;
}

}  // namespace mdreplay
