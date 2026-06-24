# 实盘因子开发交付包 (tick_feat 对齐)

> 文件说明:
> - **build_tick_feat_ORIGINAL.py** = build model 流程实际用的原版(真相源, 含 BaseModule, 需 pipeline 依赖, 仅作参照阅读)
> - **build_tick_feat_standalone.py** = 同算法的独立可跑版(无依赖, CLI), 用它算黄金答案
> - **diff_tick_feat.py** = 自验 diff 工具
> - 注: 原版头部 docstring 把 f8/f9 写成 "pdiff_demean" 是过时描述; 实际输出 = pdiff 滚动均值 pm(去均值在推理时做)


模型 16 维输入 = **f0-f9(本包黄金参照产出,1s 级)** + **4 个逐 tick 订单态/基差**(实盘按本文末算)。
目标:实盘开发者的特征实现与离线**逐位一致**(max|diff| ≈ 机器精度)。

## 文件
- `build_tick_feat_standalone.py` — **黄金参照**(离线批处理,与生产 tick_feat 完全相同)。唯一真相源。
- `diff_tick_feat.py` — diff 工具(你的输出 vs 参照)。

## 工作流(实盘开发者自验)
1. **按下面"原始数据格式"存储原始行情**(OKX okx_swap + BN binance_swap,逐事件)。
2. 用参照算标准答案:
   ```
   python3 build_tick_feat_standalone.py --raw_dir <你的原始目录> --symbol ADAUSDT --date 20260512 --out ref.parquet
   ```
   (会自动加载前 2 天暖机;pm_12h 需 ≥12h 历史)
3. 你的实盘/流式实现产出 `mine.parquet`(schema: `ts_us, f0..f9`,ts_us = **该秒起点微秒 = sec×1e6**)。
4. diff:
   ```
   python3 diff_tick_feat.py --ref ref.parquet --mine mine.parquet
   ```
   全 OK 即对齐。任何 FAIL 对照 `build_tick_feat_standalone.py` docstring 的"关键口径"排查。

## 原始数据格式(per-symbol per-day parquet,一行一事件,ts 升序)
N_LVLS=15,共 64 列(0-indexed):
| 列 | 含义 |
|---|---|
| col0 | ts 微秒 |
| col1 | side 主动方向 0买/1卖(仅成交行) |
| col2 | price ×1e8;**OB行=0**,成交行>0 |
| col3 | amount 成交量(币) |
| col4..18 | 买价 L0-14 ×1e8 |
| col19..33 | 买量 L0-14 ×1e8 |
| col34..48 | 卖价 L0-14 ×1e8 |
| col49..63 | 卖量 L0-14 ×1e8 |
- OB行: `col2==0`;成交行: `col2!=0`
- 文件名 `okx_swap_{sym}_{YYYYMMDD}.parquet`(OKX)、`binance_swap_{sym}_{YYYYMMDD}.parquet`(BN)

## ⚠️ 必须复刻的口径(否则 diff≠0)
1. 1s 桶:`ts_sec = ts // 1e6 * 1e6`。
2. **mid(trend/rv/pdiff 用)= 桶内最后一个 OB**;**imb1/imb5/spread = ts≤桶起点的最后一个 OB(=上一秒末,与 mid 不同快照!)**。
3. 成交:桶内 `signed_vol=Σ(sign·usd)`,`abs_vol=Σusd`,sign=买+1/卖−1,usd=price·amount。
4. 滚动和:`end=cs[prev_idx(q)] − cs[prev_idx(q−window·1e6−1)]`(见 `_rolling_sum`)。
5. trend 的 mid_60/300 用 as-of(`_grid_val`,ts≤q−60s 的最后网格 mid)。
6. rv:`logret=log(mid_t/mid_{t-1})`(1s网格),`rv_300s=sqrt(Σlogret²,300s)·1e4`。
7. pdiff=`(mid_okx−mid_bn_asof)/mid_bn·1e4`;pm=Σpdiff/Σ有效计数(2h/12h)。

## 模型 16 维拼装(给推理用)
sgn = +1(买单)/−1(卖单)。f0-f9 来自参照,其余 4 个逐 tick 现算:
```
fv0 = log1p(tlen)              # 本单价位队列总量(逐tick)
fv1 = max(curr_lvl, 0)         # 本单当前档位(逐tick)
fv2 = inpos * mid_usd          # 队列内我前面的量 × USD名义(逐tick)
fv3 = side                     # 0/1
fv4 = f0(imb_top1) * sgn
fv5 = f1(imb_top5) * sgn
fv6 = f2(spread_bps)
fv7 = f3(svol_ratio) * sgn
fv8 = f4(vpin)
fv9 = f5(trend60) * sgn
fv10= f6(trend300) * sgn
fv11= f7(rv_300s)
fv12= hour_utc                 # UTC 小时
fv13= dow                      # 周一0..周日6 = python weekday()
fv14= (pdiff_now − f8(pm_2h))  * sgn   # pdiff_now=瞬时基差(逐tick), pm=参照f8
fv15= (pdiff_now − f9(pm_12h)) * sgn
```
其中 `pdiff_now = (mid_okx − mid_bn)/mid_bn·1e4`,用**当前最新两所 mid**(逐 tick,不走 1s 网格)。

## 逐 tick 订单态(tlen/inpos/curr_lvl,参照未覆盖,实盘按此算)
针对本方每个挂单(price, side),每次盘口更新时:
- **curr_lvl**:
  - 买单:price≥卖1→`-(price-ask0)/step-3`;买1<price<卖1→`-1`;price≤买1→`(bid0-price)/step`(≥0)
  - 卖单:对称(price≤买1→`-(bid0-price)/step-3`;价差内→`-1`;price≥卖1→`(price-ask0)/step`)
- **tlen** = 本单价位处的盘口挂单量(在对应档找到该价的量)
- **inpos**:首次=tlen(排在整个队列后);之后当该价位队列变短(前面成交/撤)时 `inpos += (inpos/tlen_prev)*(tlen_new-tlen_prev)`(即按比例下降),再更新 tlen=tlen_new
- step = 该 symbol 最小价位步长

(可执行版见 `../live_factors.py` 的 `OrderQueueState`;但注意 live_factors 的 1s 部分尚未与参照逐位对齐,**1s 特征请以本参照为准**。)
