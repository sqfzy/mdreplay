# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么

OKX/Binance 永续 tick 因子(1s 级 f0-f9 + 实盘 16 维)的**离线黄金参照 + C++ 逐位复刻**项目。终极目的:让实盘开发者敢信自己的实现——一切围绕「实盘/C++ 与离线参照逐位一致(bit-exact)」展开。当前离线复刻已做到全 27 币 2h 数据 **「f0-f9 + mid_price」这 11 列 max|diff| = 0**(即离线参照的全部秒级输出;模型 16 维 fv 的组装与订单态属实盘线, 尚未实现)。下一步是实盘(NOTES.md 的"cpp 在线实盘"线)。

## 完整数据流

```
WS 实时行情
  → collect_okx_raw.py / collect_binance_raw.py   原始 WS 消息 → JSONL (raw_*/)
  → format_jsonl.py                                JSONL → 标准 64 列 parquet+csv (formatted*/)
  → ┌ build_tick_feat_standalone.py (python)    = 黄金参照 → ref.parquet
    ├ cpp/ tick_feat_csv (C++ 批处理, 默认零依赖)  读 csv     → mine.csv
    ├ cpp/ tick_feat     (C++ 批处理, conda arrow) 读 parquet → mine.parquet  ← 与 python 同格式
    └ cpp/ tick_feat_replay (C++ 流式引擎)         读 csv 当事件流 → mine_stream.csv  ← 实盘地基
  → diff_tick_feat.py / batch_verify.py            逐位对拍, max|diff|=0
```

## 常用命令

所有 python 用 `uv run --no-project --with <pkg>`(**必须 `--no-project`**, 否则会拽入父目录 pyproject)。

```bash
# 1) 收集 (走 clash 7890 代理, 见下方网络铁律; --dry-run 收 8s 验证)
uv run --no-project --with aiohttp python3 collect_okx_raw.py     --minutes 120 --proxy http://127.0.0.1:7890 --out raw_2h
uv run --no-project --with aiohttp python3 collect_binance_raw.py --minutes 120 --proxy http://127.0.0.1:7890 --out raw_2h

# 2) format (重建订单簿; --venue okx|binance|both; --csv 同时出 csv 喂 C++)
uv run --no-project --with pyarrow python3 format_jsonl.py --raw_dir raw_2h --venue both --out_dir formatted_2h --csv

# 3) 编译 C++ (xmake 一次只接一个 target!)
cd cpp && xmake build -y tick_feat_csv      # 默认零依赖 CSV 版
xmake build -y tick_feat                    # parquet 版, 需 conda arrow env (见铁律 4)
xmake build -y test_core && xmake run test_core   # 纯算法单元测试

# 3b) parquet 版对拍 (输入/输出都是 parquet, 与 python 同格式; 无需 round_trip)
cpp/build/linux/x86_64/release/tick_feat --raw_dir formatted_2h --symbol SOLUSDT --date 20260623 --warmup_days 0 --out mine.parquet
uv run --no-project --with pandas --with pyarrow python3 tick_feat_deliverable/diff_tick_feat.py --ref ref.parquet --mine mine.parquet

# 4) 单 symbol 对拍
cpp/build/linux/x86_64/release/tick_feat_csv --raw_dir formatted_2h --symbol SOLUSDT --date 20260623 --warmup_days 0 --out mine.csv
uv run --no-project --with pandas --with pyarrow python3 tick_feat_deliverable/build_tick_feat_standalone.py --raw_dir formatted_2h --symbol SOLUSDT --date 20260623 --warmup_days 0 --out ref.parquet
uv run --no-project --with pandas --with pyarrow python3 tick_feat_deliverable/diff_tick_feat.py --ref ref.parquet --mine mine.csv

# 5) 一键全 27 端到端 (format→C++批量→批量diff)
bash verify_2h.sh [YYYYMMDD]

# 6) 流式引擎 replay 对拍 (实盘地基; --verify 内嵌"流式==批处理 compute_day"bit-identical 检查)
cpp/build/linux/x86_64/release/tick_feat_replay --raw_dir formatted_2h --symbol SOLUSDT --date 20260623 --warmup_days 0 --out mine_stream.csv --verify
```

## 架构要点(读多个文件才能拼出的大图)

- **内层 `tick_feat_deliverable/` 是黄金参照(唯一真相源,真实算法口径)**:`build_tick_feat_standalone.py`(无依赖 CLI 版)与 `build_tick_feat_ORIGINAL.py`(生产 `BaseModule` 版)是**同一算法的两份拷贝, 改动必须同步**。`README.md` + standalone 的 docstring 是口径规范。`format_parquet_single.py` 是 tardis-CSV→parquet 的另一条老路(与本仓库的 WS→JSONL 路并存)。
- **C++ 算法核心 `cpp/src/tick_feat.hpp` 是纯逻辑 header-only**, 严格照搬 python `compute_day` 的数值流程(同 cumsum、同 as-of `prev_index`)。它**刻意设计成既能批处理、又能给实盘流式复用**:回看靠 cumsum 差分 + `prev_index`/`grid_value_asof`/`rolling_sum_asof`, 增量 append 即可流式化(实盘别另写 deque running-sum, 会偏离离线浮点路径)。
- **C++ 双路 IO**:`csv_io.hpp`(默认, 零外部依赖)与 `parquet_io.hpp`(链 conda 预编译 arrow, 见铁律 4)同名接口, `main.cpp` 用 `#ifdef USE_PARQUET` 二选一。xmake target:`tick_feat_csv`(默认)、`tick_feat`(parquet, `set_default(false)`)、`test_core`。两条 IO 路输出对 python 参照均 max|diff|=0;parquet 路更省心(二进制 double 直读, 不需 CSV 的 `round_trip` 补救)。
- **format/收集 按 venue 参数化, 不强行抽象**:OKX(books 400档增量→重建订单簿)与 Binance(depth20 全量快照, 字段 `b`/`a`, trade `m`=isBuyerMaker→方向)差异大, `format_jsonl.py` 只在 `build_rows_okx`/`build_rows_binance` 分叉, schema/输出共享(保证两所列序严格一致)。
- **C++ 流式引擎 `cpp/src/live/`(实盘地基)**:`streaming_engine.hpp` 是 `compute_day` 的**事件流增量执行形式**——同 cumsum 增量(`cs.back()+delta`)、同 Kahan、同 `prev_index` as-of, 与批处理 **bit-identical**(全 27 验证)。`event.hpp` 是事件契约(解耦数据源:replay/未来 WS/shm), `replay_feed.hpp` 把 formatted 行按 ts 归并当历史流喂。tick_feat.hpp 抽了 `ob_mid_scaled`/`imb_spread_from_levels` 给批处理与流式共用。target `tick_feat_replay`(`--verify` 内嵌"流式==批处理"对比);只对**有 OB 的秒**产行、纯成交秒丢弃、跨秒先 bn 后 okx——这三条是流式复刻离线口径的关键。

## 必须知道的铁律(踩过的坑, 违反就对不齐/连不上)

1. **bit-exact 对拍依赖两个非算法修复**(C++ 计算本身一直正确):
   - **Kahan 求和**:pandas `groupby.sum` 用 Kahan 补偿求和, C++ 桶内成交求和(`bucket_sum_trades`)必须也用 `kahan_add`, 否则每秒差 ~1e-9, 经 cumsum 大数相减放大成 vpin/svol_ratio 的 ~1e-13。
   - **`float_precision="round_trip"`**:pandas `read_csv` 默认解析器不做正确舍入, 丢 ~1 ulp(在 trend/rv 上显成 e-14)。对拍读 mine.csv 必须加它。
   - C++ 还加了 `-ffp-contract=off`(防 FMA 破坏对齐)。
2. **必须逐位复刻的口径**(见 standalone docstring):1s 桶 `ts//1e6*1e6`;**mid=桶末 OB, imb/spread=秒首 as-of OB(上一秒末, 不同快照!)**;滚动和窗口 `(q-w·1e6-1, q]`;OB 行 col2=0 / 成交行 col2>0;trade sign 买+1/卖-1, usd=price·amount;f8/f9 是 pdiff 滚动均值 pm(去均值在推理时做)。
3. **网络**:OKX/BN 的 WS 被墙, **必须走 clash mixed 端口 `http://127.0.0.1:7890`**(直连 REST 通但 WS 被 reset;旧端口 7897 已失效)。
4. **arrow 只能走隔离 conda, 别碰系统/别源码编**:三条路里只有一条通——
   - ❌ **系统 arrow**:Arch 仓库 arrow 24 / orc 与 protobuf 34 ABI 撕裂(arrow 装上即 partial upgrade)。
   - ❌ **xmake 源码编 arrow**(`add_requires("arrow")`):连环撞系统坑——① 系统 CMake 4.x 删了 `cmake_minimum_required<3.5` 兼容(内嵌 xsimd/thrift 全挂);② `find_package(Boost)` 命中系统 boost 1.90 的 `libboost_python314`(为 py3.14 编)→ 链接缺 Python 符号。是无底洞, 别走。
   - ✅ **conda-forge 预编译 libarrow**(当前方案):`micromamba create -n arrow -c conda-forge libarrow libparquet`(arrow 24.0.0), 落在隔离 env `~/micromamba/envs/arrow`, 自带配套 thrift/protobuf/boost(ABI 自洽), **全程不碰 pacman**。`xmake.lua` 的 `tick_feat` target 直接 `add_includedirs/linkdirs/rpathdirs` 指向该 env(路径可用 `ARROW_PREFIX` 覆盖)。换机器重建 env 即可;CSV 版 `tick_feat_csv` 不依赖它, 仍零依赖可用。
   - python 端 pyarrow 自带 arrow, 始终不受影响。
5. **暖机**:`pm_12h` 需 12h 历史, `trend/rv` 需多天暖机;对拍用 `--warmup_days 0` 时这些长窗口特征为 nan/未填属正常。`pm_2h` 与 `pm_12h` 在数据 ≤2h 时恒等(两窗口覆盖范围相同), 需 >2h 数据才分化。

## 目录布局

- 顶层:收集器/format/对拍脚本 + `cpp/` C++ 工程
- `tick_feat_deliverable/`(嵌套):黄金参照交付包(真相源)
- 产物:`raw_*/`(原始 JSONL) `formatted*/`(标准 parquet+csv) `mine_*/`(C++ 输出) `ref_csv/`(python 参照输出)
- git 根在父目录 `python/`, 本目录是工作单元
