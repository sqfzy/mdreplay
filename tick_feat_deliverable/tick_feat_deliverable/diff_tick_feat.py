#!/usr/bin/env python3
"""
对比 实盘/流式实现的输出 vs 黄金参照(tick_feat_reference.py), 逐特征 diff。
要求 max|diff| ≈ 机器精度 (≤1e-9 量级) 才算对齐。

两个文件都需含列: ts_us, f0..f9 (mid_price 可选)。按 ts_us 内连接对齐后比较。

用法:
  # 1) 先用黄金参照算标准答案
  python3 tick_feat_reference.py --raw_dir <RAW> --symbol ADAUSDT --date 20260512 --out ref.parquet
  # 2) 你自己的实盘实现产出 mine.parquet (同 schema: ts_us,f0..f9)
  # 3) diff
  python3 diff_tick_feat.py --ref ref.parquet --mine mine.parquet
"""
import argparse
import numpy as np, pandas as pd, pyarrow.parquet as pq

FEATS = [f"f{i}" for i in range(10)]
NAMES = {"f0":"imb_top1","f1":"imb_top5","f2":"spread_bps","f3":"svol_ratio","f4":"vpin",
         "f5":"trend60","f6":"trend300","f7":"rv_300s","f8":"pm_2h","f9":"pm_12h"}

def _load(p):
    p = str(p)
    # float_precision='round_trip': pandas 默认解析器不做正确舍入, 会丢 ~1 ulp
    d = pd.read_csv(p, float_precision="round_trip") if p.endswith(".csv") else pq.read_table(p).to_pandas()
    assert "ts_us" in d.columns, f"{p} 缺 ts_us"
    return d.set_index("ts_us")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="黄金参照 parquet")
    ap.add_argument("--mine", required=True, help="你的实现 parquet")
    ap.add_argument("--tol", type=float, default=1e-6, help="可接受 max|diff| 阈值")
    a = ap.parse_args()
    ref = _load(a.ref); mine = _load(a.mine)
    common = ref.index.intersection(mine.index)
    print(f"ref行={len(ref)}  mine行={len(mine)}  对齐(共同ts)={len(common)}")
    if len(common) == 0:
        print("!! 无共同 ts_us, 检查时间戳口径(应=该秒起点微秒 sec*1e6)"); raise SystemExit(1)
    r = ref.loc[common]; m = mine.loc[common]
    print(f"\n{'特征':14} {'max|diff|':>13} {'mean|diff|':>13} {'状态':>6}")
    allok = True
    for f in FEATS:
        if f not in m.columns: print(f"{NAMES[f]:14} {'MISSING':>13}"); allok=False; continue
        d = (r[f].values - m[f].values)
        # 忽略两边都 NaN
        msk = ~(np.isnan(r[f].values) & np.isnan(m[f].values)); d = np.abs(d[msk])
        d = d[~np.isnan(d)]
        mx = d.max() if len(d) else 0.0; mn = d.mean() if len(d) else 0.0
        ok = mx <= a.tol; allok &= ok
        print(f"{NAMES[f]:14} {mx:13.3e} {mn:13.3e} {'OK' if ok else 'FAIL':>6}")
    print("\n===== %s =====" % ("全部对齐 ✅" if allok else "存在不一致 ❌ (见上, 对照 reference docstring 的口径)"))
