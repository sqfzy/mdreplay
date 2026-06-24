# mdreplay

通用行情回放器:读录制的逐笔行情文件,按时序重放,publish 进 **gconf v2 共享内存段**,
给下游消费者(特征引擎等)当实盘 WS feed 的替身。

**独立、普适**:只依赖 gconf v2 段契约(已 vendored 进 `gconf/`)+ `toml++`/`spdlog`。
不绑任何上游/下游项目的私有格式或口径——只认「带时间戳的记录流」和「gconf 段」。

## 数据流

```
*.book.csv  ┐
*.trade.csv ┘─ 表头驱动解析 → Record 流(每文件有序)
                  │  N 路归并 (ts, 源序) 稳定确定序 + 时间窗
                  │  realtime 节奏(绝对虚拟时钟,无漂移)
                  ▼
   Book  → gconf v2 Board.slot[gid].write(BBO, latest-wins)
   Trade → gconf v2 TradeRing.publish(无损广播环)
```

## 输入契约(mdreplay 自有,与任何上游无关)

扫 `[input].dir` 下,**按文件名后缀分类**:

| 文件 | 类型 | 表头(列名自描述,按名取列,无列位魔法) |
|---|---|---|
| `*.book.csv`  | 盘口 BBO | `ts,symbol,bid_px,bid_qty,ask_px,ask_qty` |
| `*.trade.csv` | 成交     | `ts,symbol,side,px,qty` |

字段语义:
- `ts` = epoch **纳秒**(= gconf `exch_ns`)。
- `symbol` = 字符串,经 gconf `symbols.h` 映射 `global_symbol_id`;非 subset 符号该行计数跳过。
- `side`(trade)= `0` buy / `1` sell。
- `px` / `qty` / `bid_px…` = **十进制原值**。精度**沿用数据本身**:小数位数即定点 `scale`,
  逐记录写进段的 `price_scale`/`qty_scale`(mdreplay 不规定精度)。BBO 的 bid/ask 用公共 scale。
  万一某值在天然精度下越 u32,自动降 scale 保记录;整数部分都越界(不可能的高价)才跳行。
- 同一文件可含多 symbol(从列读),故文件名无需带 symbol/venue。

坏行 / 缺列 / 字段数不符 / 未知符号 → 计数跳过,不中断回放。

## 配置(`config.toml`,CLI 可覆盖)

```toml
[input]
format = "csv"            # csv(json 以后并列加)
dir    = "datas"          # 录制根目录

[replay]
realtime = 1.0            # 0~1:值=有多实时。延迟 = 真实间隔 × realtime
                          #   1=原速(默认);0.5=2× 实时;0=尽快(纯逻辑序、完全可复现)
start = ""                # 时间窗起(留空=最早),"2026-06-23 09:00:00"(UTC),可跨天
end   = ""                # 时间窗止(留空=最晚),闭区间

[[output]]                # 每条 = 一个段;format 选编码器(book/trade)
format = "book"
shm    = "/shm_bybit_lin_book_v2"
create = true             # 生产者建段(O_TRUNC 清零幂等);false=attach 既有

[[output]]
format = "trade"
shm    = "/shm_bybit_lin_trade_v2"
create = true

[log]
level        = "info"     # trace|debug|info|warn|error
progress_sec = 5
```

CLI 覆盖:`--config <path>` `--realtime <0~1>` `--dir <d>` `--start <dt>` `--end <dt>`。

## 构建 / 运行

```bash
cd mdreplay
xmake build mdreplay
xmake build test && xmake run test          # 单元 + e2e 测试

# 备测试数据(由 mdreplay 之外的 tick_feat 侧导出器生成,不属于 mdreplay):
python3 ../formatted_to_datas.py --out datas

./build/linux/x86_64/release/mdreplay --realtime 0    # 尽快回放,完全可复现
./build/linux/x86_64/release/mdreplay --realtime 1.0  # 真盘节奏
```

## 注意

- **可复现**:`realtime=0` 下,产出的 shm 内容逐字节确定(归并序固定、`exch_ns` 取数据、
  段头 `created_ns=0`)。同一输入跑两遍逐字节一致。
- **广播环不背压**:`TradeRing` 是广播环,生产者从不阻塞。`realtime=0` 全速灌时成交远超环容量
  会绕圈覆盖(消费者只能拿到最近一圈,需 resync);要让消费者无损跟上,用 `realtime=1.0` 真盘节奏。
- 段头自校验:attach 既有段时比对 magic/version/entry_size/capacity/schema_hash,不符拒启动。
- `trade.h` 是最小占位,待行情团队统一版覆盖时,加一个 `format` 对应的 sink 即可切换,核心不动。
