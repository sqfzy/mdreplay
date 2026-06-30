# mdreplay

通用**多路同步**记录回放:读录制的逐笔行情文件(csv/json),按时序重放到 **gconf v1.2.2 shm 段**
(book → `BookTickBoard` 单档 BBO 或本地 `DepthBoard` 多档;trade → 旧 `TradeRing`,临时)。主用途是给下游(特征引擎等)当实盘 WS feed 的替身。
一个 config 可配多个 `[[replays]]`,每路 `(input→output)` 在**一个进程、一条全局时钟**下同步回放、各写各段。
book 输入需带 `update_id` 列(交易所盘口更新序号)。端到端集成测试见 `test/`:`booktick_e2e.sh`(BBO)/ `depth_e2e.sh`(5 档)/ `depth25_e2e.sh`(满档 25)/ `trade_e2e.sh`(成交)/ `multi_e2e.sh`(多路同步)。

**独立、普适**:只依赖 gconf v2 段契约(vendored 进 `gconf/`)+ `toml++` / `spdlog` / `nlohmann_json`。
不绑任何上游/下游项目的私有格式或口径——只认「带时间戳的记录流」。**全 header-only,零第三方解析库**。

> CSV 解析走手写逐行读(`input/csv.hpp`:`getline` + 引号感知切分,RFC4180-lite — 支持 `"..."`/`""`
> 转义/CRLF,不支持字段内换行)。**为何不用 csv-parser**:它的 reader 每实例占 ~5MB 已提交堆,N 路归并
> 同开 N 个 → 内存随**文件数**涨;手写 `ifstream` 逐行读峰值 O(1 行)。数值走 `fixed.hpp` 定点,不碰 double。

## 设计:三段式(输入双格式 → shm 输出)

```
input/<fmt>   →   core(归并 + 节奏)   →   output(shm)
 csv / json        merge / clock / fixed       BookTickBoard(book)/ TradeRing(trade)
   ↓ 解析 Record         ↓ 全局有序 + realtime        ↓ 写 gconf v1.2.2 段
```

- **input/**:按格式可插拔的解析器(`csv.hpp` / `json.hpp`)→ Record。加格式 = 加一个文件。
- **core/**:格式无关的回放核心(record / merge 归并 / clock 节奏 / fixed 定点 / config / report)。
- **output/**:`shm.hpp` —— book→`BookTickBoard.slot[lid]`(BBO + claim_if_newer 去重)、trade→`TradeRing`(临时)。

**多路同步**:一个 config 配 N 个 `[[replays]]`,各路 `(input→output)` 在一个进程、一条全局时钟下同步回放、按 unit 路由各写各段(book/trade 同进程 = 两路;多 venue 各一路、各写各段不撞槽)。

## 输入契约(mdreplay 自有,与任何上游无关)

文件名 `*.<kind>.<format>`(如 `binance_SOLUSDT.book.csv`、`x.trade.json`)。字段:

| kind | 字段 |
|---|---|
| book(1 档 / BBO) | `ts, symbol, bid_px, bid_qty, ask_px, ask_qty` |
| book(5 档) | 上面 + `bid_px_1..4, bid_qty_1..4, ask_px_1..4, ask_qty_1..4`(level0 不带后缀) |
| trade | `ts, symbol, side, px, qty` |

- **输入格式 `csv` / `json` / `auto`**:`auto` 扫目录自动识别(仅一种格式时);两种并存或都没有 → **自描述
  报错**(说明只支持 csv/json、该怎么办),不让你干瞪眼。`--format parquet` 等能力外格式同样自描述拒绝。
- **book 档数自动识别,只接受 1 或 5**:无 `_1..` 列/键 → 1 档;`_1.._4` 齐全且无 `_5` → 5 档;
  残缺(部分档)或 >5 档 → 拒绝(csv 拒整文件 / json 跳该行),**绝不截断**;报错会**数出实际档数**
  (如"档数 100 不受支持")并告知支持范围。输出档数跟随输入。
- `ts` = epoch **纳秒**(= gconf `exch_ns`)。
- `symbol` = 字符串,经 gconf `symbols.h` 映射 `global_symbol_id`;非 subset 符号计数跳过。
- `side`(trade)= `0` buy / `1` sell。
- `px` / `qty` / `bid_px…` = **十进制原值**(json 里为**字符串**,如 `"68.84"`)。精度**沿用数据本身**:
  小数位即定点 `scale`,逐记录写进段的 `price_scale`/`qty_scale`(5 档全档共用单一 scale),mdreplay 不规定精度。

坏行 / 缺列 / 未知符号 / 数值非法 / scale 溢出 → **按原因分类计数**跳过,不中断;结束打分类汇总,
未知符号额外 WARN 去重列表(指引补 `symbols.h`)。

**csv**:首行表头(列名自描述,按名取列)。**json**:JSONL,每行一个对象,字段同上(价/量字符串)。

## 配置(`config.toml`)

顶层 `realtime`/`init_ts` 全局(一条时钟);`[[replays]]` 数组每元素一路 `(input→output)` + 本路时间窗。

```toml
realtime = 1.0      # 全局 0~1:1=原速(默认);0.5=2× 实时;0=尽快(纯逻辑序、完全可复现)
# init_ts = "2026-06-23 09:00:00"   # 可选:手动数据原点(对齐到回放开始的墙钟 now);省略=自动锚首个被回放事件

[[replays]]                                                       # 第 1 路
input  = { format = "csv", dir = "datas", kind = "book" }         # format: csv|json|auto;kind: book|trade
output = { path = "/shm_bybit_lin_book_tick", create = true }     # path: shm 段名(/开头);create: 建段/attach
start  = "" ; end = ""                                            # 本路时间窗(UTC,留空=无界,可跨天)

# [[replays]]                                                     # 第 2 路(再加一路 = 再写一个块;段名须两两不同)
# input  = { format = "csv", dir = "datas", kind = "trade" }
# output = { path = "/shm_bybit_lin_trade", create = true }

[log]
level = "info" ; progress_sec = 5
```

> 输出恒为 gconf v1.2.2 段(book depth==1→BookTickBoard、depth>1→本地 DepthBoard、trade→TradeRing)。
> book 输入需带 `update_id` 列(写进段 + claim_if_newer 去重)。

**CLI 只覆盖顶层全局项;per-replay 字段(input/output/start/end)只在 config:**

| 配置项 | CLI |
|---|---|
| `realtime` | `--realtime` |
| `init_ts` | `--init_ts`(UTC datetime 或 epoch ns) |
| `log.level` / `progress_sec` | `--log-level` / `--progress-sec` |
| `[[replays]]`(input/output/start/end) | 仅 config 文件 |
| — | `--config <path>`、`--help` |

## 构建 / 运行

```bash
cd mdreplay
xmake build mdreplay
xmake build test && xmake run test          # 单元 + e2e 测试
bash test/depth25_e2e.sh                     # 端到端(满档 25 档 book → DepthBoard)
bash test/trade_e2e.sh                       # 端到端(成交 → TradeRing,drain 读回)

# 备测试数据(由 mdreplay 之外的 tick_feat 侧导出器生成,不属于 mdreplay):
python3 ../formatted_to_datas.py --out datas             # 1 档(BBO,默认)
python3 ../formatted_to_datas.py --out datas5  --depth 5  # 5 档
python3 ../formatted_to_datas.py --out datas25 --depth 25 # 25 档(第 16-25 档为合成外推;源仅 15 档真实)

# 回放:config 里 N 个 [[replays]] 一个进程、一条时钟同步回放,各写各段。book 输入需带 update_id 列
./build/linux/x86_64/release/mdreplay --config config.toml --realtime 0
```

## 注意

- **可复现**:`realtime=0` 下,shm 段产出逐字节确定——归并序固定、`exch_ns` 取数据、
  段头 `created_ns=0`。同一输入跑两遍逐字节一致。csv/json 经定点编码 → shm 段**数值无损**(尾随零归一,如 68.80→6880@scale2)。
- **内存:流式逐行读,峰值与文件大小/总行数无关(实测 266MB 输入仅 ~0MB 已提交堆)**。每源持一个常开文件
  句柄、只缓冲 1 条记录 → 多天数据全放一个目录**单次连续回放不 OOM**(一个连续时钟,跨天节奏无缝)。
  唯一随**文件数**线性涨的是文件句柄(受 `ulimit -n`,默认 1024);单次跑 >1000 个文件时 `ulimit -n 4096`。
- **多路同步(一个进程一条时钟)**:book 与 trade(或多 venue)配成多个 `[[replays]]`,在**一个进程**里全局归并、
  共享一条时钟回放 → 天然同钟,无需跨进程协调。多 venue 各给一对独立 `(input,output)` → 各写各段,不撞 LID 槽。
  ```bash
  # config.toml: realtime=1.0 + 两个 [[replays]](book→/shm_book、trade→/shm_trade)
  mdreplay --config config.toml
  ```
- **数据原点 `init_ts`**:realtime>0 时,时钟把某数据时刻当 t0、对齐到回放开始的墙钟 now。默认 = 首个被回放事件
  (= 各路 `start`/`end` 窗口过滤后的全局最早);配顶层 `init_ts`(datetime 或 epoch ns)手动指定,早于 t0 的事件立即播。
  `realtime=0`(尽快)下时钟 no-op、`init_ts` 无意义。
- **优雅退出**:`SIGINT`(Ctrl-C)/ `SIGTERM` → 在干净边界停止回放、跑完收尾汇总(`done` + 跳过明细)、
  退出码 0(而非被硬杀的 130、丢失汇总)。`realtime>0` 的长 pacing 间隔也能在 ≤100ms 内响应中断。
- **广播环不背压**:`shm` 的 trade 段是广播环,生产者从不阻塞;`realtime=0` 全速灌时成交远超环容量
  会绕圈覆盖(消费者只能拿到最近一圈)。要让消费者无损跟上,用 `realtime=1.0` 真盘节奏。
- 段头自校验:attach 既有段时比对 magic/version/entry_size/capacity/schema_hash,不符拒启动。
- **book 按档数选段**:depth==1→`BookTickBoard`(单档 BBO,gconf v1.2.2)、depth>1→本地 `DepthBoard`(全 depth 档,
  档数 {1,5,10,15,20,25})。均按 `slot[lid]` 写、`claim_if_newer(update_id)` 去重(非递增不写段、计 `deduped` 收尾 WARN)。
- `trade` 暂留旧 vendored `TradeRing`(v1.2.2 无 market trade 段);待 gconf 统一后切新段,核心不动。
