// main.cpp — mdreplay CLI 入口。
//
// 阶段 2:load_config → 打印解析结果(验证 toml++ 集成 + 校验逻辑)。
// 编排(discover→open→replay)在阶段 3/4 接入。

#include <string>

#include <spdlog/spdlog.h>

#include "config.hpp"

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "config.toml";

  const auto cfg = mdreplay::load_config(path);
  if (!cfg) {
    spdlog::error("load config '{}' failed: {}", path, mdreplay::to_string(cfg.error()));
    return 1;
  }

  spdlog::info("config: dir='{}' format={} realtime={} window=[{}..{}] scale(p{},q{})", cfg->dir,
               cfg->input_format, cfg->realtime, cfg->start_ns, cfg->end_ns,
               cfg->scale.default_price, cfg->scale.default_qty);
  for (const auto& o : cfg->outputs)
    spdlog::info("  output: format={} shm={} create={}", o.format, o.shm, o.create);

  return 0;
}
