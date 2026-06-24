-- mdreplay —— 通用行情回放 → gconf v2 shm。独立工程,只依赖 vendored gconf 契约 + spdlog/toml++。
set_project("mdreplay")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_warnings("allextra")

-- gconf v2 段契约已 vendored 进 mdreplay/gconf(保项目独立,不外伸 cpp/ 或别处)
add_includedirs("gconf/include", "src")

-- csv-parser 5.3.0(RFC4180:引号/转义/CRLF)源码已 vendored 进 third_party/csvparser。
-- xmake-repo 的 csvparser 包误标 headeronly,实为多文件需编译,故 vendor 源码 + 自编静态库(下方 target)。
-- CSV_ENABLE_THREADS=0:单线程解析,确定可复现、免 pthread;该宏 PUBLIC 影响内部布局,
--   必须 lib 与消费方完全一致 → 全局 define。头作 sysinclude 引入,免 -Wextra 对第三方头刷屏。
-- 数值仍走 fixed.hpp 定点,不碰 double,保 bit-exact。
add_defines("CSV_ENABLE_THREADS=0")
add_sysincludedirs("third_party/csvparser/include")

add_requires("spdlog", {system = true})  -- 系统 pkg-config(同 cpp 工程约定)
add_requires("toml++")                    -- header-only,xmake 包管理拉取
add_requires("nlohmann_json")             -- header-only,json 输入/输出

-- vendored csv-parser 静态库:编 9 个实现 .cpp(权威集见上游 internal/CMakeLists.txt)。
-- 单独 target + 关警告,避免第三方源污染主工程的 allextra。
target("csvparser")
    set_kind("static")
    set_warnings("none")
    add_includedirs("third_party/csvparser/include", "third_party/csvparser/include/internal")
    add_files("third_party/csvparser/include/internal/*.cpp",
              "third_party/csvparser/include/internal/parser/*.cpp")

target("mdreplay")
    set_kind("binary")
    add_files("src/main.cpp")
    add_packages("spdlog", "toml++", "nlohmann_json")
    add_deps("csvparser")

-- 单元测试:fixed / clock / merge / config / e2e / csv-quoting
target("test")
    set_kind("binary")
    set_default(false)
    add_files("test/test_main.cpp")
    add_packages("spdlog", "toml++", "nlohmann_json")
    add_deps("csvparser")
