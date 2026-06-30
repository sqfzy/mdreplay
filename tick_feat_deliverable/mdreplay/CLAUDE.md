# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么

`mdreplay` 是一个**独立、普适**的逐笔行情回放器:读录制的行情文件(csv/json 输入),按时序重放到 **gconf v1.2.2 shm 段**。主用途是给下游(特征引擎等)当实盘 WS feed 的替身。

> **gconf v1.2.2 迁移(进行中)**:输出已从旧 v2 段(csv/json)迁到 shm——book 单档 BBO → `BookTickBoard`(v1.2.2 生产段,`/shm_*_book_tick`),输出**只剩 shm**(csv/json 输出已删)。**trade 暂留旧 vendored `TradeRing`**(v1.2.2 无 market trade 段,待 gconf 统一)。**book 输入必填 `update_id` 列**(交易所盘口更新序号真值,写进段、claim_if_newer 去重)。符号用 v1.2.2 共享 LID 层(SOLUSDT=19;旧版是 21)。集成测试见 `test/booktick_e2e.sh`(BBO)/ `test/depth_e2e.sh`(多档)。
>
> **多档(深档)已落地**:book 档数支持 `{1,5,10,15,20,25}`。`depth==1` → `BookTickBoard`(BBO,契约不变);`depth>1` → **`DepthBoard`(`src/output/depth_board.h`,mdreplay 本地自定义多档段**,gconf v1.2.2 无多档段、经决策本地定义,复用 gconf `SegHeader`/`seqlock`/`schema_fnv` 框架,单一定长 25 档槽 + `depth` 字段 + `claim_if_newer` 去重,按 LID 索引)。**下游须按 `mdreplay::DepthBoardSlot` 布局消费——非官方 gconf 段**。导出器 `formatted_to_datas.py --depth {1,5,10,15,20,25}`(formatted 源仅 15 档**真实**;20/25 的第 16-25 档为**合成外推**——整数空间按最后一档真实间距线性延伸、薄盘尾档置 0、绝不为负,仅供端到端验证引擎 25 档能力)。集成测试:`test/depth25_e2e.sh`(满档 25)/ `test/trade_e2e.sh`(成交)。

**铁律 — 零上游耦合**:本项目只认「带时间戳的记录流」,**绝不绑任何上游/下游的私有格式或口径**(连 venue 差异、tick_feat 因子口径都在它之外)。它**只依赖** gconf v2 段契约(vendored 进 `gconf/`)+ `toml++` / `spdlog` / `nlohmann_json`,**CSV 解析手写(`input/csv.hpp`),零第三方解析库**。改动时若发现自己在写 venue 专属或某下游专属逻辑,基本是放错了地方。

> 注:本目录是父仓库 `tick_feat_deliverable/` 的**独立子工程**(git 根在更上层的 `python/`)。父 CLAUDE.md 讲的是 tick_feat 因子复刻,**与本项目算法无关** —— mdreplay 不算因子,只搬运记录。

## 构建 / 运行 / 测试

xmake 工程,C++23,`src/` **全 header-only**(几乎全是 `.hpp`,仅 `main.cpp` / `test_main.cpp` 是 TU);**零第三方解析库依赖**。

```bash
cd mdreplay
xmake build mdreplay                          # 主程序
xmake build test && xmake run test            # 单元 + e2e 测试(无框架,任一断言失败退出非零)
bash test/mem_check.sh                        # 内存回归守门(操作级:跑二进制采 RssAnon,验流式不随数据量涨)
bash test/booktick_e2e.sh                     # 端到端(BBO):拟真 1 档 book → BookTickBoard shm → 另进程按 v1.2.2 契约读回核对
bash test/depth_e2e.sh                        # 端到端(多档):拟真 5 档 book → DepthBoard shm → 另进程按 DepthBoard 布局逐档读回核对
bash test/depth25_e2e.sh                      # 端到端(满档 25):拟真 25 档 book → DepthBoard shm → 另进程逐档(L0/L12/L24)读回核对
bash test/trade_e2e.sh                        # 端到端(成交):拟真 trade → TradeRing shm → 另进程 drain 逐条读回核对(价量+scale+GID+side+顺序)
bash test/multi_e2e.sh                        # 端到端(多路):一个 config 两路(book+trade)单进程同钟 → 另进程同时读回两段核对路由
bash test/multi_venue_e2e.sh                  # 端到端(同 kind 多 venue):两路 book 各写各段 + per-replay 窗口 → 验无串段、窗口裁末行
bash test/repro_e2e.sh                        # 端到端(可复现守门):同输入跑两遍 → shm 段逐字节一致(铁律 1 自动化)
```

- **两层测试**:`xmake run test`(`test_main.cpp`,纯逻辑单元 + e2e,进程内);`test/mem_check.sh`(操作级,跑二进制读 `/proc` 采峰值 `RssAnon`,守住「~0MB 已提交堆与数据量无关」这一核心卖点——纯逻辑单测够不到进程级内存)。改输入加载/缓冲策略后务必跑后者。

- 产物路径:`build/linux/x86_64/release/mdreplay`。
- 依赖:`spdlog` 走系统 pkg-config(`{system = true}`);`toml++` / `nlohmann_json` 由 xmake 包管理拉取。**无静态库 target、无 vendored 解析器**。
- **CSV 解析手写(`input/csv.hpp`)**:`getline` + 引号感知切分(RFC4180-lite:`"..."` 包裹/`""` 转义/CRLF,不支持字段内换行)。**为何不用 csv-parser**:其 reader 每实例占 ~5MB 已提交堆,N 路归并同开 N 个 → 内存随**文件数**涨;手写 `ifstream` 逐行读峰值 O(1 行),实测 266MB 输入仅 ~0MB 已提交堆(见铁律 8)。无引号行走零拷贝快路(string_view 直指行缓冲),含引号行去引号到 owned 缓冲。CSV 数值走 `fixed.hpp` 定点,**不碰 double**(保 bit-exact)。曾用 csv-parser 5.3.0,因上述内存问题于流式改造时移除(实测手写解析对 csv-parser 全路径逐字节一致)。
- **单测无 per-test 过滤**:`test/test_main.cpp` 是单一二进制,`main()` 顺序调 `test_fixed` / `test_clock` / `test_signal` / `test_merge`(含多 unit 路由 + per-unit 窗口 + 跨 unit 同 ts tiebreak)/ `test_config`([[replays]] 数组解析 + path 重复/缺项/窗口/format 边界)/ `test_e2e` / `test_file_io` / `test_csv_quoting` / `test_skip_reasons` / `test_book_depth_of`(档数判定 {1,5,10,15,20,25})/ `test_book_depth5` / `test_depth_board`(DepthBoard 多档写读 + 去重 + 满档 25)/ `test_init_ts`(手动数据原点)等。要只跑一个,临时在 `main()` 注释掉其余调用(别引入测试框架,保持同 cpp 工程 `test_core` 的极简风格)。

```bash
# 回放:一个 config 的 N 个 [[replays]] 在一个进程、一条时钟里同步回放,各写各段。book 输入需带 update_id 列
./build/linux/x86_64/release/mdreplay --config config.toml --realtime 0
```

测试数据 `datas/*.{book,trade}.csv` 由 mdreplay **之外**的导出器(`../formatted_to_datas.py`,`--depth {1,5,10,15,20,25}` 选档数;20/25 的深档为合成外推)生成,不属于本项目契约。

## 配置:[[replays]] 数组 + 全局 flag 覆盖

`config.toml` 是基线。**顶层全局**项有同名 CLI 覆盖:`realtime`→`--realtime`、`init_ts`→`--init_ts`、`log.level`→`--log-level`、`log.progress_sec`→`--progress-sec`;`--config <path>` 换配置、`--help` 看全部。**per-replay 字段(input/output/start/end)无 CLI 覆盖,只在 config 配置**(多路本就 config 驱动)。`main.cpp::load_with_overrides` 先 `load_config` 再叠全局覆盖,**覆盖后复校验**。

每个 `[[replays]]` 是一个自包含单元:`input = { format, dir, kind }` + `output = { path(shm 段名,/开头), create(建段/attach) }` + 本路 `start`/`end` 时间窗。输出恒为 shm。校验:replays≥1、各 output.path 非空且**两两不同**、各 kind∈{book,trade}、各 start≤end。

## 架构:三段式(输入双格式 → shm 输出)(读多个文件才能拼出的大图)

```
input/<fmt>  →  core(归并 + 节奏)  →  output(shm)
 csv / json       merge / clock / window     BookTickBoard(book)/ TradeRing(trade)
   ↓ 解析 Record       ↓ 全局有序 + 限速          ↓ 写 gconf v1.2.2 段
```

**`Record`(`core/record.hpp`)是三段解耦的命脉** —— 格式无关的中间语:输入 seam 产出它、输出 seam 消费它、核心只搬运它。价/量以**定点 mantissa(`uint32 ×10^scale`)**承载,**绝不走 double**;精度沿用数据本身(由字符串小数位推得)。book 携带 `depth`({1,5,10,15,20,25})+ `bid/ask_px/qty` 为 `array<u32, kMaxDepth=25>`(0=最优档);**全档共用单一 `price_scale`/`qty_scale`**。`depth=1` 即 BBO,与旧契约逐字一致。

- **`input/`**:按格式可插拔的**流式**解析器。`discover.hpp` 扫目录挑 `*.<kind>.<fmt>` 并**按路径排序**(固定源序 = 确定性归并的基础);`csv.hpp`(手写 getline+切分)/ `json.hpp`(nlohmann)表头/字段驱动解析,**各自实现 `Source`**(`StreamingCsvSource`/`StreamingJsonSource`,`source.hpp` 的 `peek/advance` 游标契约)——`advance()` 才惰性读下一行、**只缓冲 1 条**,峰值内存与文件大小无关;`record_build.hpp` 是 csv/json **共用**的「字段串 → Record」逻辑(symbol→gid + fixed 编码,失败返回**分类的** `SkipReason`)。坏/未知行在**消费期**(advance)按原因归账到 `SkipStats`(故 `main` 的 `log_summary` 在 replay 后打)。**book 档数自动识别,只接受 `{1,5,10,15,20,25}`**:无 `_1..` 列/键→1;`_1.._(d-1)` 连续齐 → d 档(如 `_1.._4`→5、`_1.._9`→10、`_1.._24`→25);残缺/中缺/跳档/越界→拒(csv 拒整文件 / json 跳行),**绝不截断**(`book_depth_of` 是唯一真相源)。**加一种输入格式 = 加一个文件 + 在 main 的 `load_sources` 分叉一行**。
- **`core/`**:格式无关回放核心。`merge.hpp` 用小顶堆做 N 路归并,序 = `(ts_ns, 源序)` 稳定 tiebreak(**通用规则,不含任何 venue 口径**);**每条产出带 `unit`(= 哪个 [[replays]])供路由**,并**按 unit 的 per-replay 窗口** `[start,end]` 过滤(全局 sources 按 unit-major 拼接 → 源下标天然编码 `(unit,源内序)`,单 unit 时退化为旧行为逐字节一致)。`clock.hpp` 绝对虚拟时钟(延迟 = `(ts - ts0) × realtime`,**不累积漂移**,steady_clock;**数据原点 ts0**:默认锚首个被回放事件,配顶层 `init_ts` 则手动定 t0、早于 t0 的事件偏移钳 0;pacing 分块睡可被停止信号 ≤100ms 打断);`window.hpp` 时间窗 sentinel(`kNoStart/kNoEnd`);`fixed.hpp` 定点编解码;`skip.hpp` 跳过分原因统计(`SkipStats`);`config.hpp` toml 解析 + 启动期校验;`report.hpp` 进度观测。
- **`output/`**:只剩 shm(`shm.hpp`),统一 `Sink::write(Record)` + `on_finish()` 接口(`sink.hpp`)。含 `ShmSegment`(POSIX shm RAII + 建段写头/连段自校验)+ `BookSink`(写 `BookTickBoard.slot[lid]` BBO 最优档,`claim_if_newer(update_id)` 去重)+ `DepthSink`(写本地 `DepthBoard.slot[lid]` 全 depth 档,同去重)+ `TradeSink`(老 `TradeRing.publish`,无损广播环,临时)。`main::open_output`(按一路的 kind+depth 选段):book depth==1→BookTickBoard、depth>1→DepthBoard、trade→TradeRing。

> **每路档数流向**:`main` 对每个 `[[replays]]` 走 `load_one_replay` —— 先 `load_sources`(book 自动识别档数→写进 `Record.depth`)、再 `detect_depth` 取首条档数、据此 `open_output` 选段;各路源拼进全局列表(记 `unit`)。即每路「先载入、后开输出」,全部载完再全局归并回放。

### 多入多出:N 路同步回放(关键约束)

一个 config 可配**多个 `[[replays]]`**,每路是自包含的 `(input → output)` 单元(各自 kind/dir/format/段/时间窗)。N 路在**一个进程、一条全局时钟**下回放:全局归并成一条 `(ts, 源序)` 有序流,每条带 `unit` 标记 → 路由到 `outputs[unit]` 的 Sink。各路仍保留自己的契约(book 走 Board 段 latest-wins、trade 走广播环无损),只是**共享时钟**——book+trade 同进程同钟喂下游 = 配两路;**多 venue 各给一对独立单元** → 各写各段、天然不撞 LID 槽。

- **路由按 unit 不按 kind**:两路即便同 kind(如 binance/okx 两路 book)也各写各段,故 `output.path` 校验**两两不同**(防互覆盖)。
- **一条时钟的数据原点 ts0**:默认 = 首个被回放事件(= 各路 per-replay 窗口过滤后的全局最早,窗口过滤在归并层、早于时钟,故 ts0 自动尊重 start/end)。顶层 `init_ts`(可选,datetime 或 epoch ns)手动把某 data-ts 当 t0、对齐到回放开始的墙钟 now;早于 t0 的事件偏移钳 0(立即播)。`realtime=0`(尽快)下时钟 no-op、`init_ts` 无意义、逐字节可复现。
- **可复现**:同输入两遍逐字节一致(归并序固定、unit-major 拼接确定、`realtime=0` 无时钟扰动)。改归并序 / 拼接顺序 / 定点编码即可能破坏,改完重跑两遍对比字节。

## 必须知道的铁律(踩了就对不齐/连不上)

1. **可复现是核心卖点**:`realtime=0`(尽快)下产出**逐字节确定** —— 归并序固定、`exch_ns` 取数据值、段头 `created_ns=0`。同一输入跑两遍逐字节一致;csv/json 经定点编码 → shm 段**数值无损**(尾随零归一,68.80→6880@scale2)。改动**任何**影响归并序、定点编码、段头初值的代码,都可能破坏这一保证 —— 改完务必重跑两遍对比字节。
2. **book 必带 update_id + 去重可观测**:book 输入必有 `update_id` 列/字段(交易所盘口序号真值);写段经 `claim_if_newer`,**非递增(≤已记录)的不写段**——BookSink 计 `deduped` 数,`on_finish` >0 则 WARN(clean 回放恒 0,>0 = 输入 update_id 有重复/倒退)。别静默吞掉去重(`done` 计数含被去重的,WARN 是唯一信号)。
3. **广播环不背压**:trade 段是广播环,生产者从不阻塞;`realtime=0` 全速灌时成交远超环容量会**绕圈覆盖**(消费者只拿到最近一圈)。要让消费者无损跟上,用 `realtime=1.0` 真盘节奏。
4. **错误分层 + 跳过分类(`core/error.hpp` + `core/skip.hpp`)**:可恢复的**逐行**错误由调用方 **按原因计数跳过、不断流**(`SkipStats` 分 `Malformed/BadTimestamp/BadField/UnknownSymbol/BadNumber/ScaleOverflow`,结束打分类汇总 + 未知符号去重 WARN);不可恢复的**启动期**错误(配置/建段)经 `Result<Error>` 上抛 main 转非零退出。`make_*_record` 返回 `expected<Record, SkipReason>` 把「为什么跳」从构建层带到归账层。新增逻辑沿用这条分界,别让坏行中断回放、也别让坏配置带病启动、别把跳过又折叠回单一计数。
5. **shm attach 自校验**:`create=false` 连既有段时比对 magic/version/entry_size/capacity/schema_hash(`SchemaDrift` 容忍,其余拒启动)。`create=true` 走 `O_TRUNC` 清零重建(幂等)。
6. **gconf 是 vendored 契约(v1.2.2),别外伸**:段布局 / 符号表全来自 `gconf/include/`(v1.2.2 子集 + 临时保留的旧 `trade.h`/`event.h`)。符号:`gid_of`(GID,trade 段用)/ `lid_of`(LID,book 段 slot 索引)都查 `gconf::sym::kGidNames`(v1.2.2 共享层,LID==GID 恒等);非 subset 符号计数跳过。**book BBO 段 = `BookTickBoard`(单档,`kBoardSchemaHash`),按 `slot[lid]` 写、`claim_if_newer` 去重**。**例外:多档段 `DepthBoard` 经决策由 mdreplay 本地定义**(`src/output/depth_board.h`,gconf v1.2.2 无多档段)——它复用 gconf 框架原语但 Slot/Board 是 mdreplay-local 契约,不在 gconf 内。需要其它新段(market trade 等)时看 `gconf/include/gconf/shm/v2/`,别引父仓库 `cpp/`;v1.2.2 现无 market trade 段 → trade 暂用旧 TradeRing。
7. **book 输出按档数选段 + 必填 `update_id`**:`depth==1` → `BookTickBoard`(BBO,L0);`depth>1` → `DepthBoard`(全 depth 档,`kDepthBoardSchemaHash`)。两段都按 `slot[lid]` 写、`claim_if_newer` 去重。book 输入 csv/json **必须带 `update_id` 列/字段**(交易所盘口更新序号真值,写进段 + claim_if_newer 去重;**不可用 ts 合成**,语义不同)。导出器 `formatted_to_datas.py` 已补 `update_id` 列(**per-symbol 单调计数器**——formatted 无交易所真值 seqno,合成单调序号让 book 开箱可跑,严格递增 → 去重恒 0;要真值须改 `format_jsonl.py` 从 raw WS 带出)。**档数 20/25 时第 16-25 档为合成外推**(formatted 源仅 15 档真实;整数空间按最后一档真实间距线性延伸、薄盘尾档置 0、绝不为负)。集成测试:`test/booktick_e2e.sh`(BBO)/ `test/depth_e2e.sh`(5 档)/ `test/depth25_e2e.sh`(满档 25)/ `test/trade_e2e.sh`(成交),均自带 update_id。
8. **流式逐行读,峰值内存与文件大小/总行数无关**:`StreamingCsvSource`/`StreamingJsonSource` 各持一个常开 `ifstream`、`advance()` 才解析下一行、只缓冲 1 条 → 实测 266MB 输入仅 ~0MB 已提交堆。**多天数据全放一目录、单次连续回放不 OOM**(一个连续时钟、跨天 realtime 节奏无缝);随**文件数**线性涨的只有文件句柄(受 `ulimit -n`,>1000 文件时 `ulimit -n 4096`)。**别**把"整文件入 vector / 攒进容器再回放"加回来——那会让内存回到 O(总行数),正是流式改造要消灭的。CSV 用手写解析而非 csv-parser 也是为此(后者每 reader ~5MB 堆 × N)。**`test/mem_check.sh` 守这条铁律**:跑 1× vs 3× 数据采 `RssAnon`,涨了就红;改加载/缓冲后必跑。