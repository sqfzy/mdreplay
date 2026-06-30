#pragma once
// error.hpp — mdreplay 统一错误体系。一次定死,贯穿全程(不留 anyhow/占位)。
//
// 约定:可恢复的逐行错误(解析/编码单行失败)由调用方 WARN+计数+跳行,不向上抛断流;
// 不可恢复的启动期错误(配置/建段)经 Result<T> 上抛到 main 转非零退出。

#include <expected>
#include <string_view>

namespace mdreplay {

enum class Error {
  ConfigNotFound, // 配置文件不存在
  ConfigParse,    // toml 语法解析失败
  ConfigInvalid,  // 配置字段非法(realtime 越界 / format 未知 / kind 非法)
  FileOpen,       // 输入文件打不开
  OutputOpen,     // 输出文件打不开(csv/json sink)
  CsvParse,       // CSV 单行数值解析失败
  CsvSchema,      // CSV 表头缺必需列
  BookDepthUnsupported,  // book 档数不在 {1,5,10,15,20,25}(自适应只接受这些,不截断)
  ShmOpen,        // shm_open / ftruncate / mmap 失败
  SegMismatch,    // attach 既有段时 seg_check 不符(magic/version/abi)
  ScaleOverflow,  // value×10^scale 越 u32 上限
  SymbolUnknown,  // symbol 不在 gconf subset
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept {
  switch (e) {
    case Error::ConfigNotFound: return "config file not found";
    case Error::ConfigParse:   return "config parse error";
    case Error::ConfigInvalid: return "config invalid";
    case Error::FileOpen:      return "input file open failed";
    case Error::OutputOpen:    return "output file open failed";
    case Error::CsvParse:      return "csv row parse error";
    case Error::CsvSchema:     return "csv header missing required column";
    case Error::BookDepthUnsupported: return "book 档数不受支持(只接受 {1,5,10,15,20,25},不截断)";
    case Error::ShmOpen:       return "shm open/mmap failed";
    case Error::SegMismatch:   return "segment header check mismatch";
    case Error::ScaleOverflow: return "value*10^scale overflows u32";
    case Error::SymbolUnknown: return "symbol not in gconf subset";
  }
  return "?";
}

}  // namespace mdreplay
