set_project("tick_feat_cpp")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_warnings("allextra")
-- 禁用 FMA 合并, 让浮点严格按源码逐步舍入, 对齐 numpy(纯 SSE) → diff 逼近机器精度
add_cxflags("-ffp-contract=off")

-- spdlog: 系统已装 (Arch spdlog 1.17), 走 pkg-config
add_requires("spdlog", {system = true})

-- arrow: 不用系统包(Arch arrow 24 与系统 protobuf 34 ABI 撕裂; 系统 boost/cmake4 又使源码编译连环失败)。
-- 改用 conda-forge 预编译 libarrow(隔离 micromamba env, 自带配套 thrift/protobuf/boost, ABI 自洽),
-- 不碰 pacman, 不编译任何 C++ 依赖 → 解锁 parquet 直读, 对齐 python 单一格式。
-- env 路径可用 ARROW_PREFIX 覆盖; 默认指向 `micromamba create -n arrow` 的安装位置。
local arrow_prefix = os.getenv("ARROW_PREFIX") or (os.getenv("HOME") .. "/micromamba/envs/arrow")

-- 算法核心纯逻辑单元测试 (不依赖 arrow)
target("test_core")
    set_kind("binary")
    set_default(false)
    add_files("tests/test_core.cpp")
    add_includedirs("src")
    add_packages("spdlog")

-- 流式引擎单元测试 (边界用例 + 流式==批处理 bit-identical 对比)
target("test_streaming")
    set_kind("binary")
    set_default(false)
    add_files("tests/test_streaming.cpp")
    add_includedirs("src")
    add_packages("spdlog")

-- 默认复刻版: 零外部依赖, 读标准 CSV → 算 f0-f9 → 写 CSV (系统 arrow 不可用)
target("tick_feat_csv")
    set_kind("binary")
    add_files("src/main.cpp")
    add_includedirs("src")
    add_packages("spdlog")

-- 流式引擎 replay 版: 读 formatted 标准行 → 流式增量结算 → CSV; --verify 比批处理
target("tick_feat_replay")
    set_kind("binary")
    add_files("src/live_replay_main.cpp")
    add_includedirs("src")
    add_packages("spdlog")

-- parquet 直读版: 链接 conda 预编译 libarrow(隔离 env), 读 parquet → 算 f0-f9 → 写 parquet
target("tick_feat")
    set_kind("binary")
    set_default(false)
    add_files("src/main.cpp")
    add_includedirs("src")
    add_packages("spdlog")
    add_defines("USE_PARQUET")
    add_includedirs(path.join(arrow_prefix, "include"))
    add_linkdirs(path.join(arrow_prefix, "lib"))
    add_links("parquet", "arrow")           -- parquet 在前(依赖 arrow)
    add_rpathdirs(path.join(arrow_prefix, "lib"))   -- 运行时定位 .so 及其 conda 传递依赖
