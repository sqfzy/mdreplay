# mdreplay

通用**单入单出**记录回放:读录制的逐笔行情文件,按时序重放到一个去向(gconf v2 **shm 段** /
**csv** / **json** 文件)。主用途是给下游(特征引擎等)当实盘 WS feed 的替身;也可做格式转换/落盘/调试。

**独立、普适**:只依赖 gconf v2 段契约 + csv-parser(均已 vendored:`gconf/`、`third_party/csvparser/`)
+ `toml++` / `spdlog` / `nlohmann_json`。不绑任何上游/下游项目的私有格式或口径——只认「带时间戳的记录流」。

> CSV 解析走 vendored 的 csv-parser 5.3.0(RFC4180:引号/转义/CRLF)。注意它**不是 header-only**(xmake-repo
> 的 `csvparser` 包误标 headeronly,实为多文件需编译),故 vendor 源码 + `xmake.lua` 里编成静态库 target。

## 设计:三段式 + 双格式轴

```
input/<fmt>   →   core(归并 + 节奏)   →   output/<dest>
 csv / json        merge / clock / fixed       shm(board/ring) / csv / json
   ↓ 解析 Record         ↓ 全局有序 + realtime        ↓ 编码 Record
```

- **input/**:按格式可插拔的解析器(`csv.hpp` / `json.hpp`)→ Record。加格式 = 加一个文件。
- **core/**:格式无关的回放核心(record / merge 归并 / clock 节奏 / fixed 定点 / config / report)。
- **output/**:按去向的编码器(`shm.hpp` 段 / `csv.hpp` 文件 / `json.hpp` 文件)。

**单入单出**:一次只回放一种(book 或 trade)。要 book 和 trade 都喂,跑两遍(改 `--kind` 与 `--output.*`)。

## 输入契约(mdreplay 自有,与任何上游无关)

文件名 `*.<kind>.<format>`(如 `binance_SOLUSDT.book.csv`、`x.trade.json`)。字段:

| kind | 字段 |
|---|---|
| book(1 档 / BBO) | `ts, symbol, bid_px, bid_qty, ask_px, ask_qty` |
| book(5 档) | 上面 + `bid_px_1..4, bid_qty_1..4, ask_px_1..4, ask_qty_1..4`(level0 不带后缀) |
| trade | `ts, symbol, side, px, qty` |

- **book 档数自动识别,只接受 1 或 5**:无 `_1..` 列/键 → 1 档;`_1.._4` 齐全且无 `_5` → 5 档;
  残缺(部分档)或 >5 档 → 拒绝(csv 拒整文件 / json 跳该行),**绝不截断**。输出档数跟随输入。
- `ts` = epoch **纳秒**(= gconf `exch_ns`)。
- `symbol` = 字符串,经 gconf `symbols.h` 映射 `global_symbol_id`;非 subset 符号计数跳过。
- `side`(trade)= `0` buy / `1` sell。
- `px` / `qty` / `bid_px…` = **十进制原值**(json 里为**字符串**,如 `"68.84"`)。精度**沿用数据本身**:
  小数位即定点 `scale`,逐记录写进段的 `price_scale`/`qty_scale`(5 档全档共用单一 scale),mdreplay 不规定精度。

坏行 / 缺列 / 未知符号 / 数值非法 / scale 溢出 → **按原因分类计数**跳过,不中断;结束打分类汇总,
未知符号额外 WARN 去重列表(指引补 `symbols.h`)。

**csv**:首行表头(列名自描述,按名取列)。**json**:JSONL,每行一个对象,字段同上(价/量字符串)。

## 配置(`config.toml`,所有项都有同名/同路径 CLI)

```toml
[input]
format = "csv"      # csv | json —— 输入文件格式
dir    = "datas"
kind   = "book"     # book | trade —— 一次只回放一种

[replay]
realtime = 1.0      # 0~1:1=原速(默认);0.5=2× 实时;0=尽快(纯逻辑序、完全可复现)
start = "" ; end = ""   # 时间窗(UTC,"YYYY-MM-DD HH:MM:SS",留空=无界,可跨天)

[output]
format = "shm"      # shm | csv | json —— 去向
path   = "/shm_bybit_lin_book_v2"   # 去向定位:shm=段名(/开头);csv|json=文件路径(如 out.book.csv)
create = true                       # 仅 format=shm:建段(O_TRUNC 幂等)/ false=attach
```

> `[output]` 的 `path` 是统一的「去向定位」:`format=shm` 时是段名,`format=csv|json` 时是文件路径。
> book 档数不在配置里(由输入自动识别 1/5)。

| 配置项 | CLI |
|---|---|
| `input.format` / `dir` / `kind` | `--format` / `--dir` / `--kind` |
| `replay.realtime` / `start` / `end` | `--realtime` / `--start` / `--end` |
| `output.format` / `path` / `create` | `--output.format` / `--output.path` / `--output.create` |
| `log.level` / `progress_sec` | `--log-level` / `--progress-sec` |
| — | `--config <path>`、`--help` |

## 构建 / 运行

```bash
cd mdreplay
xmake build mdreplay
xmake build test && xmake run test          # 单元 + e2e 测试

# 备测试数据(由 mdreplay 之外的 tick_feat 侧导出器生成,不属于 mdreplay):
python3 ../formatted_to_datas.py --out datas            # 1 档(BBO,默认)
python3 ../formatted_to_datas.py --out datas5 --depth 5 # 5 档

# 回放 book → shm(尽快、可复现);trade 同理另跑一遍。档数(1/5)由输入自动识别
./build/linux/x86_64/release/mdreplay --kind book  --output.format shm --output.path /shm_bybit_lin_book_v2  --realtime 0
./build/linux/x86_64/release/mdreplay --kind trade --output.format shm --output.path /shm_bybit_lin_trade_v2 --realtime 0

# 5 档:同命令喂 5 档输入,自动识别 depth=5 → shm DepthBoard 段
./build/linux/x86_64/release/mdreplay --kind book --dir datas5 --output.format shm --output.path /shm_book5 --realtime 0

# 格式转换:book csv → json 文件
./build/linux/x86_64/release/mdreplay --kind book --output.format json --output.path out.book.json --realtime 0
```

## 注意

- **可复现**:`realtime=0` 下,产出(shm 段 / 文件)逐字节确定——归并序固定、`exch_ns` 取数据、
  段头 `created_ns=0`。同一输入跑两遍逐字节一致。csv↔json↔shm 经定点往返**数值无损**(尾随零归一,如 68.80→68.8)。
- **广播环不背压**:`shm` 的 trade 段是广播环,生产者从不阻塞;`realtime=0` 全速灌时成交远超环容量
  会绕圈覆盖(消费者只能拿到最近一圈)。要让消费者无损跟上,用 `realtime=1.0` 真盘节奏。
- 段头自校验:attach 既有段时比对 magic/version/entry_size/capacity/schema_hash,不符拒启动。
- **book→shm 按档数选段**:1 档写 BBO `Board`(`BoardSlot` 64B),5 档写 `DepthBoard`(`DepthSlot` 128B,
  5 档价量)。两者靠 `entry_size` + 独立 `schema_hash` 区分,消费者据此自校验。
- `trade.h` 是最小占位,待行情团队统一版覆盖时,加一个 `output` 编码器即可切换,核心不动。
