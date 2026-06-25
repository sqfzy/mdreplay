# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么

`mdreplay` 是一个**独立、普适**的逐笔行情回放器:读录制的行情文件(csv/json),按时序重放到一个去向(gconf v2 **shm 段** / **csv** / **json** 文件)。主用途是给下游(特征引擎等)当实盘 WS feed 的替身;也可做格式转换 / 落盘 / 调试。

**铁律 — 零上游耦合**:本项目只认「带时间戳的记录流」,**绝不绑任何上游/下游的私有格式或口径**(连 venue 差异、tick_feat 因子口径都在它之外)。它**只依赖** gconf v2 段契约(vendored 进 `gconf/`)+ `toml++` / `spdlog` / `nlohmann_json`,**CSV 解析手写(`input/csv.hpp`),零第三方解析库**。改动时若发现自己在写 venue 专属或某下游专属逻辑,基本是放错了地方。

> 注:本目录是父仓库 `tick_feat_deliverable/` 的**独立子工程**(git 根在更上层的 `python/`)。父 CLAUDE.md 讲的是 tick_feat 因子复刻,**与本项目算法无关** —— mdreplay 不算因子,只搬运记录。

## 构建 / 运行 / 测试

xmake 工程,C++23,`src/` **全 header-only**(几乎全是 `.hpp`,仅 `main.cpp` / `test_main.cpp` 是 TU);**零第三方解析库依赖**。

```bash
cd mdreplay
xmake build mdreplay                          # 主程序
xmake build test && xmake run test            # 单元 + e2e 测试(无框架,任一断言失败退出非零)
```

- 产物路径:`build/linux/x86_64/release/mdreplay`。
- 依赖:`spdlog` 走系统 pkg-config(`{system = true}`);`toml++` / `nlohmann_json` 由 xmake 包管理拉取。**无静态库 target、无 vendored 解析器**。
- **CSV 解析手写(`input/csv.hpp`)**:`getline` + 引号感知切分(RFC4180-lite:`"..."` 包裹/`""` 转义/CRLF,不支持字段内换行)。**为何不用 csv-parser**:其 reader 每实例占 ~5MB 已提交堆,N 路归并同开 N 个 → 内存随**文件数**涨;手写 `ifstream` 逐行读峰值 O(1 行),实测 266MB 输入仅 ~0MB 已提交堆(见铁律 8)。无引号行走零拷贝快路(string_view 直指行缓冲),含引号行去引号到 owned 缓冲。CSV 数值走 `fixed.hpp` 定点,**不碰 double**(保 bit-exact)。曾用 csv-parser 5.3.0,因上述内存问题于流式改造时移除(实测手写解析对 csv-parser 全路径逐字节一致)。
- **单测无 per-test 过滤**:`test/test_main.cpp` 是单一二进制,`main()` 顺序调 `test_fixed` / `test_clock` / `test_signal` / `test_merge` / `test_config` / `test_e2e` / `test_file_io` / `test_csv_quoting` / `test_skip_reasons` / `test_book_depth5` 等。要只跑一个,临时在 `main()` 注释掉其余调用(别引入测试框架,保持同 cpp 工程 `test_core` 的极简风格)。

```bash
# 回放 book → shm(尽快、可复现);trade 需另跑一遍(单入单出)。book 档数(1/5)由输入自动识别
./build/linux/x86_64/release/mdreplay --kind book  --output.format shm --output.path /shm_bybit_lin_book_v2  --realtime 0
./build/linux/x86_64/release/mdreplay --kind trade --output.format shm --output.path /shm_bybit_lin_trade_v2 --realtime 0

# 格式转换:book csv → json 文件(文件输出忽略 realtime,恒按尽快)
./build/linux/x86_64/release/mdreplay --kind book --output.format json --output.path out.book.json
```

测试数据 `datas/*.{book,trade}.csv` 由 mdreplay **之外**的导出器(`../formatted_to_datas.py`,`--depth 1|5` 选档数)生成,不属于本项目契约。

## 配置:CLI 全覆盖 config.toml

`config.toml` 是基线,**每一项都有同名/同路径 CLI 覆盖**(镜像 toml 表路径,如 `[output].path` → `--output.path`)。`main.cpp::load_with_overrides` 先 `load_config` 再逐项覆盖,**覆盖后复校验**(CLI 可能注入非法值)。`--config <path>` 换配置文件,`--help` 看全部。

`[output]` 是 **`format`(shm/csv/json)+ `path`(去向定位:shm=段名 /开头,csv|json=文件路径)+ `create`(仅 shm)**。`path` 是统一的去向(`shm` 与旧 `path` 已合并);book 档数不在配置里——由输入自动识别 1/5。

## 架构:三段式 + 双格式轴(读多个文件才能拼出的大图)

```
input/<fmt>  →  core(归并 + 节奏)  →  output/<dest>
 csv / json       merge / clock / window     shm(board/ring) / csv / json
   ↓ 解析 Record       ↓ 全局有序 + 限速          ↓ 编码 Record
```

**`Record`(`core/record.hpp`)是三段解耦的命脉** —— 格式无关的中间语:输入 seam 产出它、输出 seam 消费它、核心只搬运它。价/量以**定点 mantissa(`uint32 ×10^scale`)**承载,**绝不走 double**;精度沿用数据本身(由字符串小数位推得)。book 携带 `depth`(1 或 5)+ `bid/ask_px/qty` 为 `array<u32, kMaxDepth=5>`(0=最优档);**全档共用单一 `price_scale`/`qty_scale`**。`depth=1` 即 BBO,与旧契约逐字一致。

- **`input/`**:按格式可插拔的**流式**解析器。`discover.hpp` 扫目录挑 `*.<kind>.<fmt>` 并**按路径排序**(固定源序 = 确定性归并的基础);`csv.hpp`(手写 getline+切分)/ `json.hpp`(nlohmann)表头/字段驱动解析,**各自实现 `Source`**(`StreamingCsvSource`/`StreamingJsonSource`,`source.hpp` 的 `peek/advance` 游标契约)——`advance()` 才惰性读下一行、**只缓冲 1 条**,峰值内存与文件大小无关;`record_build.hpp` 是 csv/json **共用**的「字段串 → Record」逻辑(symbol→gid + fixed 编码,失败返回**分类的** `SkipReason`)。坏/未知行在**消费期**(advance)按原因归账到 `SkipStats`(故 `main` 的 `log_summary` 在 replay 后打)。**book 档数自动识别,只接受 1 或 5**:无 `_1..` 列/键→1;`_1.._4` 齐且无 `_5`→5;残缺/>5→拒(csv 拒整文件 / json 跳行),**绝不截断**。**加一种输入格式 = 加一个文件 + 在 main 的 `load_sources` 分叉一行**。
- **`core/`**:格式无关回放核心。`merge.hpp` 用小顶堆做 N 路归并,序 = `(ts_ns, 源序)` 稳定 tiebreak(**通用规则,不含任何 venue 口径**);`clock.hpp` 绝对虚拟时钟(延迟 = `(ts - ts0) × realtime`,锚定首事件**不累积漂移**);`window.hpp` 时间窗 sentinel(`kNoStart/kNoEnd`);`fixed.hpp` 定点编解码;`skip.hpp` 跳过分原因统计(`SkipStats`:per-reason 计数 + 未知符号去重采样,结束 `log_summary` 打分类汇总 + WARN);`config.hpp` toml 解析 + 启动期校验;`report.hpp` 进度观测。
- **`output/`**:按去向的编码器,统一 `Sink::write(Record)` 接口(`sink.hpp`)。`shm.hpp` 含 `ShmSegment`(POSIX shm RAII + 建段写头/连段自校验)+ `BookSink`(写 BBO `Board.slot[gid]` 的最优档,latest-wins)+ `DepthBookSink`(写 5 档 `DepthBoard.slot[gid]`)+ `TradeSink`(`TradeRing.publish`,无损广播环);`csv.hpp` / `json.hpp` 文件输出(按 `record.depth` 逐档,经 `fixed.hpp::to_decimal` 还原精度)。`main::open_output` 据**探测到的档数**选 `Board`(1)/`DepthBoard`(5)。**加一种去向 = 加一个 Sink + 在 main 的 `open_output` 分支**。

> **档数流向**:`main` 先 `load_sources`(book 自动识别档数→写进 `Record.depth`),再 `detect_depth(sources)` 取首条记录的档数,据此 `open_output`(选段类型、定 csv 表头)。即「先载入、后开输出」——顺序不能反。

### 单入单出(关键约束)

一次只回放**一种** kind(book 或 trade),由 `--kind` 选定;book 与 trade 都要喂下游就**跑两遍**(改 `--kind` 与 `--output.*`)。这是刻意的解耦:book 走 Board 段(状态、latest-wins),trade 走广播环(流、无损),两者契约不同,不强行合并。

## 必须知道的铁律(踩了就对不齐/连不上)

1. **可复现是核心卖点**:`realtime=0`(尽快)下产出**逐字节确定** —— 归并序固定、`exch_ns` 取数据值、段头 `created_ns=0`。同一输入跑两遍逐字节一致;csv↔json↔shm 经定点往返**数值无损**(尾随零归一,68.80→68.8)。改动**任何**影响归并序、定点编码、段头初值的代码,都可能破坏这一保证 —— 改完务必重跑两遍对比字节。
2. **文件输出忽略 realtime**:`main` 检测到 `output.format != shm` 且 `realtime > 0` 时强制归 0 并 WARN(原速写文件白等没意义)。
3. **广播环不背压**:trade 段是广播环,生产者从不阻塞;`realtime=0` 全速灌时成交远超环容量会**绕圈覆盖**(消费者只拿到最近一圈)。要让消费者无损跟上,用 `realtime=1.0` 真盘节奏。
4. **错误分层 + 跳过分类(`core/error.hpp` + `core/skip.hpp`)**:可恢复的**逐行**错误由调用方 **按原因计数跳过、不断流**(`SkipStats` 分 `Malformed/BadTimestamp/BadField/UnknownSymbol/BadNumber/ScaleOverflow`,结束打分类汇总 + 未知符号去重 WARN);不可恢复的**启动期**错误(配置/建段)经 `Result<Error>` 上抛 main 转非零退出。`make_*_record` 返回 `expected<Record, SkipReason>` 把「为什么跳」从构建层带到归账层。新增逻辑沿用这条分界,别让坏行中断回放、也别让坏配置带病启动、别把跳过又折叠回单一计数。
5. **shm attach 自校验**:`create=false` 连既有段时比对 magic/version/entry_size/capacity/schema_hash(`SchemaDrift` 容忍,其余拒启动)。`create=true` 走 `O_TRUNC` 清零重建(幂等)。
6. **gconf 是 vendored 契约,别外伸**:符号表 / 段布局全来自 `gconf/include/`(已拷进本项目保独立)。symbol→gid 用 `gconf::sym::kNames`,非 subset 符号计数跳过。需要新段类型时优先看 `gconf/include/gconf/shm/v2/`,别去引用父仓库 `cpp/` 的任何东西。5 档的 `DepthBoard`/`DepthSlot`(`depth_board.h`)就是这样新增的段:同 `SegKind::Board`,靠 `entry_size`(128B)+ 独立 `kDepthBoardSchemaHash` 与 BBO `Board` 区分。
7. **book 档数只 1 或 5、自动识别、不截断**:由输入(csv 表头 / json 每行键)判定,`level0` 列名不带后缀(= 旧 BBO,零迁移),`_1.._4` 为高档;残缺或 >5 → 拒绝,绝不悄悄用部分档。5 档全档共用单一 `price_scale`/`qty_scale`(对齐 `DepthSlot`)。改档相关逻辑务必同步 input 判定 / Record 数组 / 三个 output sink / `DepthBoard` 五处。
8. **流式逐行读,峰值内存与文件大小/总行数无关**:`StreamingCsvSource`/`StreamingJsonSource` 各持一个常开 `ifstream`、`advance()` 才解析下一行、只缓冲 1 条 → 实测 266MB 输入仅 ~0MB 已提交堆。**多天数据全放一目录、单次连续回放不 OOM**(一个连续时钟、跨天 realtime 节奏无缝);随**文件数**线性涨的只有文件句柄(受 `ulimit -n`,>1000 文件时 `ulimit -n 4096`)。**别**把"整文件入 vector / 攒进容器再回放"加回来——那会让内存回到 O(总行数),正是流式改造要消灭的。CSV 用手写解析而非 csv-parser 也是为此(后者每 reader ~5MB 堆 × N)。