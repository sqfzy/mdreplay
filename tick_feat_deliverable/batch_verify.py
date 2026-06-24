#!/usr/bin/env python3
"""批量对拍: 每个 symbol 用 python 黄金参照(OKX+BN)算 ref, 与 C++ 输出 CSV diff。

ref = compute_day([okx_swap_*.parquet], [binance_swap_*.parquet], date)  # 有 BN 则算 f8/f9
mine = {mine_dir}/{symbol}.csv  (C++ tick_feat_csv 产出)
"""
import argparse
import glob
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tick_feat_deliverable"))
from build_tick_feat_standalone import compute_day  # noqa: E402

FEATS = [f"f{i}" for i in range(10)]
TOL = 1e-6


def verify_one(okx_pq: str, bn_pq: str | None, mine_csv: str, date: str):
    bn = [bn_pq] if bn_pq and os.path.exists(bn_pq) else []
    ref = compute_day([okx_pq], bn, date)
    mine = pd.read_csv(mine_csv, float_precision="round_trip")  # 避免默认解析器丢 ~1 ulp
    r = ref.set_index("ts_us"); m = mine.set_index("ts_us")
    common = r.index.intersection(m.index)
    worst = 0.0
    for f in FEATS:
        d = np.abs(r.loc[common, f].values - m.loc[common, f].values)
        d = d[~np.isnan(d)]
        if len(d):
            worst = max(worst, float(d.max()))
    has_bn = bool(bn)
    return len(ref), len(common), worst, has_bn


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--formatted", default="formatted_2h")
    ap.add_argument("--mine", default="mine_2h")
    ap.add_argument("--date", default="20260623")
    a = ap.parse_args()

    files = sorted(glob.glob(f"{a.formatted}/okx_swap_*_{a.date}.parquet"))
    if not files:
        print(f"无 OKX parquet: {a.formatted}/okx_swap_*_{a.date}.parquet"); return 1
    print(f"{'symbol':12} {'ref行':>6} {'对齐':>6} {'BN':>3} {'max|diff|':>12} {'状态':>6}")
    all_ok = True
    worst_overall = 0.0
    for okx_pq in files:
        symbol = os.path.basename(okx_pq).split("_")[2]
        bn_pq = f"{a.formatted}/binance_swap_{symbol}_{a.date}.parquet"
        mine_csv = f"{a.mine}/{symbol}.csv"
        if not os.path.exists(mine_csv):
            print(f"{symbol:12} {'缺 mine':>30}"); all_ok = False; continue
        ref_rows, common, worst, has_bn = verify_one(okx_pq, bn_pq, mine_csv, a.date)
        ok = worst <= TOL
        all_ok &= ok
        worst_overall = max(worst_overall, worst)
        print(f"{symbol:12} {ref_rows:6d} {common:6d} {'Y' if has_bn else 'N':>3} "
              f"{worst:12.3e} {'OK' if ok else 'FAIL':>6}")
    print(f"\n最大 max|diff| (全 symbol 全特征) = {worst_overall:.3e}")
    print("===== %s =====" % ("全部逐位对齐 ✅" if all_ok else "存在不一致 ❌"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
