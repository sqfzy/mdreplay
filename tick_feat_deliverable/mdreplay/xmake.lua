-- mdreplay —— 通用行情回放 → gconf v2 shm。独立工程,只依赖 vendored gconf 契约 + spdlog/toml++。
set_project("mdreplay")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_warnings("allextra")

-- gconf v2 段契约已 vendored 进 mdreplay/gconf(保项目独立,不外伸 cpp/ 或别处)
add_includedirs("gconf/include", "src")

add_requires("spdlog", {system = true})  -- 系统 pkg-config(同 cpp 工程约定)
add_requires("toml++")                    -- header-only,xmake 包管理拉取
add_requires("nlohmann_json")             -- header-only,json 输入/输出

target("mdreplay")
    set_kind("binary")
    add_files("src/main.cpp")
    add_packages("spdlog", "toml++", "nlohmann_json")

-- 单元测试:fixed / clock / merge / config / e2e
target("test")
    set_kind("binary")
    set_default(false)
    add_files("test/test_main.cpp")
    add_packages("spdlog", "toml++", "nlohmann_json")
