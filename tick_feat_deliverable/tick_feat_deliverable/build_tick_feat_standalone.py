#!/usr/bin/env python3
"""
======================================================================
 tick_feat 黄金参照 (GOLDEN REFERENCE) — 离线批处理, 与生产 tick_feat 完全一致
======================================================================
用途: 实盘开发者按"原始数据格式"存数据后, 用本脚本算出"标准答案" (f0-f9 + mid),
      再把自己的实盘/流式实现的输出与之 diff (见 diff_tick_feat.py)。
      要求 max|diff| ≈ 机器精度。本文件是唯一真相源, 不要凭描述重写。

模型 16 维输入 = 本文件产出的 f0-f9 (按方向乘 sgn / 去均值) + 4 个逐tick订单态(另见README)。

────────────────────────────────────────────────────────────────────
 原始数据格式 (per-symbol per-day parquet, 一行一个事件, 按 ts 升序)
────────────────────────────────────────────────────────────────────
 列(0-indexed), N_LVLS=15:
   col0  = ts        事件时间戳 (微秒)
   col1  = side      成交主动方向: 0=主动买, 1=主动卖 (仅成交行有意义)
   col2  = price     成交价 ×1e8 ; **OB快照行此列=0**, 成交行此列>0
   col3  = amount    成交量 (币, 非张)
   col4 ..18  = 买价 L0..L14  ×1e8     (col4=bp0=买1价)
   col19..33  = 买量 L0..L14  ×1e8     (col19=买1量)
   col34..48  = 卖价 L0..L14  ×1e8     (col34=ap0=卖1价)
   col49..63  = 卖量 L0..L14  ×1e8     (col49=卖1量)
 → OB行判定: col2==0 ; 成交行判定: col2!=0
 文件名: {venue}_{symbol}_{YYYYMMDD}.parquet
   OKX(挂单所/主venue) = okx_swap_* ; BN(算基差) = binance_swap_*

────────────────────────────────────────────────────────────────────
 ⚠️ 关键口径 (实盘必须逐位复刻, 否则 diff 不为 0)
────────────────────────────────────────────────────────────────────
 1) 1s 网格: ts_sec = (ts // 1_000_000) * 1_000_000  (向下取整到秒, 单位微秒)
 2) **mid (用于 trend/rv/pdiff)** = 该秒桶 [sec,sec+1) 内 **最后一个 OB** 的 mid。
 3) **imb1/imb5/spread** = ts ≤ sec(秒起点) 的 **最后一个 OB** (= 上一秒末快照)。
    注意: 与 mid 取的不是同一个快照 (mid用桶末, imb/spread用桶首)! 这是既有口径, 必须照做。
 4) 成交: 每秒桶内 signed_vol=Σ(sign·usd), abs_vol=Σusd ; sign=买+1/卖-1 ; usd=price·amount。
 5) 滚动和 (svol60/avol60/svol10/avol10/lr2_300/pdiff窗) 用 _rolling_sum:
    end=cs[prev_idx(q)], begin=cs[prev_idx(q - window*1e6 - 1)] ; 窗口约 (q-window, q]。
 6) trend 的 mid_60/mid_300 用 _grid_val(as-of): 取 ts ≤ (q-60s) 的最后一个网格 mid。
 7) rv: logret = log(mid_t / mid_{t-1}) 在 1s 网格上; rv_300s = sqrt(Σlogret²,300s)·1e4。
 8) pdiff = (mid_okx - mid_bn_asof)/mid_bn·1e4, bn_mid 用 as-of(prev_idx); pm_2h/12h = Σpdiff/Σ有效计数。
 9) **暖机**: pm_12h 需 12h 历史 → 算某天前必须加载前 1~2 天 (warmup_days>=1; 生产=2)。

输出列: ts_us, f0..f9, mid_price
   f0 imb_top1  f1 imb_top5  f2 spread_bps  f3 svol_ratio(60s净占比)  f4 vpin(10s)
   f5 trend60   f6 trend300  f7 rv_300s     f8 pm_2h(基差2h均值)      f9 pm_12h
======================================================================
"""
import sys, argparse, calendar
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np, pandas as pd, pyarrow as pq_pa  # noqa
import pyarrow.parquet as pq

N_LVLS  = 15
GRID_US = 1_000_000        # 1s 桶 (生产口径)


def _date_add(d, n): return (datetime.strptime(d, "%Y%m%d") + timedelta(days=n)).strftime("%Y%m%d")

def _load_pq(path):
    try: return pq.read_table(str(path)).to_pandas()
    except Exception: return None

def _ob_arrays(df):
    """取所有 OB 行(col2==0), 返回 ts + bp0/ap0 + 前5档量。"""
    ob = df[df.iloc[:, 2] == 0].copy()
    if ob.empty: return None
    ob = ob.sort_values(ob.columns[0]).reset_index(drop=True)
    ts  = ob.iloc[:, 0].values.astype(np.int64)
    bp0 = ob.iloc[:, 4].values / 1e8
    ap0 = ob.iloc[:, 4 + 2 * N_LVLS].values / 1e8
    ba  = np.stack([ob.iloc[:, 4 + N_LVLS + i].values / 1e8 for i in range(5)], axis=1)
    aa  = np.stack([ob.iloc[:, 4 + 3 * N_LVLS + i].values / 1e8 for i in range(5)], axis=1)
    return {"ts": ts, "bp0": bp0, "ap0": ap0, "ba": ba, "aa": aa}

def _grid_raw(df):
    """1s 网格: mid=桶末, signed_vol/abs_vol=桶内成交和。"""
    ob = df[df.iloc[:, 2] == 0].copy(); tr = df[df.iloc[:, 2] != 0].copy()
    if ob.empty: return pd.DataFrame()
    ob = ob.sort_values(ob.columns[0])
    ob["ts_sec"] = (ob.iloc[:, 0].values // GRID_US) * GRID_US
    ob["mid"]    = (ob.iloc[:, 4].values + ob.iloc[:, 4 + 2 * N_LVLS].values) / 2.0 / 1e8
    grid_mid = ob.groupby("ts_sec")["mid"].last().reset_index()
    if not tr.empty:
        tr = tr.copy()
        tr["ts_sec"] = (tr.iloc[:, 0].values // GRID_US) * GRID_US
        tr["sign"]   = np.where(tr.iloc[:, 1].values == 0, 1.0, -1.0)
        tr["usd"]    = tr.iloc[:, 3].values * tr.iloc[:, 2].values / 1e8
        tr["svol"]   = tr["sign"] * tr["usd"]
        grp = tr.groupby("ts_sec").agg(signed_vol=("svol", "sum"), abs_vol=("usd", "sum")).reset_index()
    else:
        grp = pd.DataFrame(columns=["ts_sec", "signed_vol", "abs_vol"])
    grid = grid_mid.merge(grp, on="ts_sec", how="left").fillna(0.0)
    return grid[["ts_sec", "mid", "signed_vol", "abs_vol"]].sort_values("ts_sec").reset_index(drop=True)

def _grid_mid_only(df):
    ob = df[df.iloc[:, 2] == 0].copy()
    if ob.empty: return pd.DataFrame()
    ob = ob.sort_values(ob.columns[0])
    ob["ts_sec"] = (ob.iloc[:, 0].values // GRID_US) * GRID_US
    ob["mid"]    = (ob.iloc[:, 4].values + ob.iloc[:, 4 + 2 * N_LVLS].values) / 2.0 / 1e8
    return ob.groupby("ts_sec")["mid"].last().reset_index()

def _prev_idx(s, q):  return np.searchsorted(s, q, side="right") - 1
def _grid_val(ts_grid, mid_grid, q):
    idx = _prev_idx(ts_grid, q); valid = idx >= 0
    out = np.full(len(q), np.nan); out[valid] = mid_grid[idx[valid]]; return out
def _rolling_sum(cs, ts_grid, q, window_s):
    idx_e = _prev_idx(ts_grid, q); idx_b = _prev_idx(ts_grid, q - window_s * 1_000_000 - 1)
    valid = idx_e >= 0; out = np.zeros(len(q))
    end_v = np.where(valid, cs[np.clip(idx_e, 0, len(cs) - 1)], 0.0)
    beg_v = np.where(idx_b >= 0, cs[np.clip(idx_b, 0, len(cs) - 1)], 0.0)
    out[valid] = end_v[valid] - beg_v[valid]; return out


def compute_day(okx_paths, bn_paths, target_date):
    """算 target_date 一天的 tick_feat. okx_paths/bn_paths 需含暖机前日。返回 DataFrame。"""
    target_lo = calendar.timegm(datetime.strptime(target_date, "%Y%m%d").timetuple()) * 1_000_000
    target_hi = target_lo + 86400 * 1_000_000
    grids_raw = []
    for p in sorted(okx_paths):
        df = _load_pq(p)
        if df is None or df.empty: continue
        ob = _ob_arrays(df); g = _grid_raw(df); del df
        if ob is None or g.empty: continue
        ts_sec_arr = g["ts_sec"].values
        ob_idx = _prev_idx(ob["ts"], ts_sec_arr); valid = ob_idx >= 0; ci = ob_idx.clip(0)   # imb/spread 用 as-of 秒首
        bp0_ = np.where(valid, ob["bp0"][ci], np.nan); ap0_ = np.where(valid, ob["ap0"][ci], np.nan)
        mid_ = (bp0_ + ap0_) / 2.0; ba_ = ob["ba"][ci]; aa_ = ob["aa"][ci]
        bid1 = ba_[:, 0]; ask1 = aa_[:, 0]; bid5 = ba_.sum(1); ask5 = aa_.sum(1)
        d1 = bid1 + ask1; d5 = bid5 + ask5
        g = g.copy()
        g["imb1"]   = np.where(valid & (d1 > 0), (bid1 - ask1) / d1, 0.0)
        g["imb5"]   = np.where(valid & (d5 > 0), (bid5 - ask5) / d5, 0.0)
        g["spread"] = np.where(valid & (mid_ > 0), (ap0_ - bp0_) / mid_ * 1e4, 0.0)
        del ob; grids_raw.append(g)
    if not grids_raw: return None
    bn_mids = []
    for p in sorted(bn_paths):
        df = _load_pq(p)
        if df is None or df.empty: continue
        gm = _grid_mid_only(df); del df
        if not gm.empty: bn_mids.append(gm)
    grid = pd.concat(grids_raw, ignore_index=True).sort_values("ts_sec").reset_index(drop=True)
    grid = grid.groupby("ts_sec", as_index=False).last()
    mid_arr = grid["mid"].values.astype(float)
    lr = np.zeros(len(mid_arr)); lr[1:] = np.log(np.where(mid_arr[:-1] > 0, mid_arr[1:] / mid_arr[:-1], 1.0))
    ts_arr = grid["ts_sec"].values
    cs_svol = np.cumsum(grid["signed_vol"].values); cs_avol = np.cumsum(grid["abs_vol"].values); cs_lr2 = np.cumsum(lr ** 2)
    imb1_arr = grid["imb1"].values; imb5_arr = grid["imb5"].values; spr_arr = grid["spread"].values
    pdiff_arr = np.zeros(len(ts_arr)); pdiff_cnt = np.zeros(len(ts_arr))
    if bn_mids:
        bn_grid = pd.concat(bn_mids, ignore_index=True).sort_values("ts_sec")
        bn_grid = bn_grid.groupby("ts_sec")["mid"].last().reset_index()
        bn_ts = bn_grid["ts_sec"].values; bn_mid = bn_grid["mid"].values.astype(float)
        bi = _prev_idx(bn_ts, ts_arr); bv = bi >= 0; bc = bi.clip(0)
        bn_m = np.where(bv, bn_mid[bc], np.nan)
        safe = bv & (bn_m > 0) & np.isfinite(mid_arr) & (mid_arr > 0)
        pdiff_arr = np.where(safe, (mid_arr - bn_m) / bn_m * 1e4, 0.0); pdiff_cnt = safe.astype(float)
    cs_pdiff = np.cumsum(pdiff_arr); cs_pcnt = np.cumsum(pdiff_cnt)
    day_mask = (ts_arr >= target_lo) & (ts_arr < target_hi)
    if not day_mask.any(): return None
    day_ts = ts_arr[day_mask]
    svol60 = _rolling_sum(cs_svol, ts_arr, day_ts, 60); avol60 = _rolling_sum(cs_avol, ts_arr, day_ts, 60)
    svol10 = _rolling_sum(cs_svol, ts_arr, day_ts, 10); avol10 = _rolling_sum(cs_avol, ts_arr, day_ts, 10)
    lr2_300 = _rolling_sum(cs_lr2, ts_arr, day_ts, 300)
    mid_now = mid_arr[day_mask]
    mid_60 = _grid_val(ts_arr, mid_arr, day_ts - 60 * 1_000_000); mid_300 = _grid_val(ts_arr, mid_arr, day_ts - 300 * 1_000_000)
    svol_ratio = np.where(avol60 > 1e-9, svol60 / avol60, 0.0)
    vpin = np.where(avol10 > 1e-9, np.abs(svol10) / avol10, -1.0)
    trend60 = np.where(mid_now > 0, (mid_now - mid_60) / mid_now * 1e4, 0.0)
    trend300 = np.where(mid_now > 0, (mid_now - mid_300) / mid_now * 1e4, 0.0)
    rv_300s = np.sqrt(np.maximum(lr2_300, 0.0)) * 1e4
    def _pm(w):
        s_e = _rolling_sum(cs_pdiff, ts_arr, day_ts, w); n_e = _rolling_sum(cs_pcnt, ts_arr, day_ts, w)
        return np.where(n_e > 0, s_e / n_e, 0.0)
    pm_2h = _pm(7200); pm_12h = _pm(43200)
    return pd.DataFrame({"ts_us": day_ts.astype(np.int64),
        "f0": imb1_arr[day_mask], "f1": imb5_arr[day_mask], "f2": spr_arr[day_mask],
        "f3": svol_ratio, "f4": vpin, "f5": trend60, "f6": trend300, "f7": rv_300s,
        "f8": pm_2h, "f9": pm_12h, "mid_price": mid_now})


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="算某币某天的 tick_feat 黄金参照")
    ap.add_argument("--raw_dir", required=True, help="原始 parquet 目录")
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--date", required=True, help="目标日 YYYYMMDD")
    ap.add_argument("--warmup_days", type=int, default=2, help="加载前N天暖机(pm_12h需>=1, 建议2)")
    ap.add_argument("--okx_prefix", default="okx_swap")
    ap.add_argument("--bn_prefix", default="binance_swap")
    ap.add_argument("--out", default="tick_feat_ref.parquet")
    a = ap.parse_args()
    rd = Path(a.raw_dir)
    days = [_date_add(a.date, -i) for i in range(a.warmup_days, -1, -1)]
    okx = [str(rd / f"{a.okx_prefix}_{a.symbol}_{d}.parquet") for d in days if (rd / f"{a.okx_prefix}_{a.symbol}_{d}.parquet").exists()]
    bn  = [str(rd / f"{a.bn_prefix}_{a.symbol}_{d}.parquet")  for d in days if (rd / f"{a.bn_prefix}_{a.symbol}_{d}.parquet").exists()]
    df = compute_day(okx, bn, a.date)
    if df is None: print("ERROR: empty"); sys.exit(1)
    pq.write_table(pq_pa.Table.from_pandas(df, preserve_index=False), a.out)
    print(f"OK rows={len(df)} -> {a.out}")
    print(df.head(3).to_string())
