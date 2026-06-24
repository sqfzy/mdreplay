"""BuildTickFeatModule: 为每个 symbol 按天计算 1s-grid tick 特征，输出 parquet 供 C++ FeatureLoad 读取。

处理单元: (symbol, date)，加载 warmup_days+1 天数据保证 rolling 特征正确，只输出当天行。
所有 (symbol, date) 任务 100 并发。完成后按 symbol 合并为单文件供 FeatureLoad。

10 列特征（f0..f7 基于 OKX；f8/f9 基于 OKX+BN 双边 pdiff，均无符号，C++ 乘 sgn）:
  f0 imb1              OB 1档量比
  f1 imb5              OB 5档量比
  f2 spread_bps        买卖价差 bps
  f3 svol_ratio        60s 有符号成交量比
  f4 vpin              10s VPIN
  f5 trend60           60s 价格趋势 bps
  f6 trend300          300s 价格趋势 bps
  f7 rv_300s           300s 已实现波动率
  f8 pdiff_demean_2h   价差去均值 2h  (pdiff_now - rolling_mean_2h)
  f9 pdiff_demean_12h  价差去均值 12h (pdiff_now - rolling_mean_12h)

输出：{output_dir}/{symbol}.parquet  列: [ts_us(int64), f0..f9(float64)]
"""
from __future__ import annotations

import logging
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

from pipeline.core import BaseModule, Context
from run_filledgeearb.src.steps import load_config

logger = logging.getLogger("pipeline.run_filledgeearb.tick_feat")

N_LVLS    = 15
WARMUP_S  = 43200   # 12h warmup 覆盖 pdiff_mean_12h 所需历史


# ── 日期工具 ──────────────────────────────────────────────────────────────

def _date_add(d: str, days: int) -> str:
    return (datetime.strptime(d, "%Y%m%d") + timedelta(days=days)).strftime("%Y%m%d")


def _date_from_path(p: Path) -> str:
    stem = p.stem
    for part in reversed(stem.replace(".", "_").split("_")):
        if len(part) == 8 and part.isdigit():
            return part
    return ""


# ── parquet 加载 ──────────────────────────────────────────────────────────

def _load_pq(path: Path) -> pd.DataFrame | None:
    if not path.exists():
        return None
    try:
        return pq.read_table(str(path)).to_pandas()
    except Exception:
        return None


# ── 单天 OB 快照（ts, bp0, ap0, bid1, ask1, bid5, ask5）─────────────────

def _ob_arrays(df: pd.DataFrame) -> dict | None:
    ob = df[df.iloc[:, 2] == 0].copy()
    ob = ob.sort_values(ob.columns[0]).reset_index(drop=True)
    if ob.empty:
        return None
    ts  = ob.iloc[:, 0].values.astype(np.int64)
    bp0 = ob.iloc[:, 4].values / 1e8
    ap0 = ob.iloc[:, 4 + 2*N_LVLS].values / 1e8
    ba  = np.stack([ob.iloc[:, 4 + N_LVLS + i].values / 1e8 for i in range(5)], axis=1)
    aa  = np.stack([ob.iloc[:, 4 + 3*N_LVLS + i].values / 1e8 for i in range(5)], axis=1)
    return {"ts": ts, "bp0": bp0, "ap0": ap0, "ba": ba, "aa": aa}


# ── 1s-grid 原始值（mid + signed/abs trade volume）───────────────────────

def _grid_raw(df: pd.DataFrame) -> pd.DataFrame:
    ob = df[df.iloc[:, 2] == 0].copy()
    tr = df[df.iloc[:, 2] != 0].copy()
    if ob.empty:
        return pd.DataFrame()
    ob = ob.sort_values(ob.columns[0])
    ob["ts_sec"] = (ob.iloc[:, 0].values // 1_000_000) * 1_000_000
    ob["mid"]    = (ob.iloc[:, 4].values + ob.iloc[:, 4+2*N_LVLS].values) / 2.0 / 1e8
    grid_mid     = ob.groupby("ts_sec")["mid"].last().reset_index()

    if not tr.empty:
        tr = tr.copy()
        tr["ts_sec"] = (tr.iloc[:, 0].values // 1_000_000) * 1_000_000
        tr["sign"]   = np.where(tr.iloc[:, 1].values == 0, 1.0, -1.0)
        tr["usd"]    = tr.iloc[:, 3].values * tr.iloc[:, 2].values / 1e8
        tr["svol"]   = tr["sign"] * tr["usd"]
        grp = tr.groupby("ts_sec").agg(
            signed_vol=("svol", "sum"), abs_vol=("usd", "sum")
        ).reset_index()
    else:
        grp = pd.DataFrame(columns=["ts_sec", "signed_vol", "abs_vol"])

    grid = grid_mid.merge(grp, on="ts_sec", how="left").fillna(0.0)
    return grid[["ts_sec", "mid", "signed_vol", "abs_vol"]].sort_values("ts_sec").reset_index(drop=True)


def _grid_mid_only(df: pd.DataFrame) -> pd.DataFrame:
    ob = df[df.iloc[:, 2] == 0].copy()
    if ob.empty:
        return pd.DataFrame()
    ob = ob.sort_values(ob.columns[0])
    ob["ts_sec"] = (ob.iloc[:, 0].values // 1_000_000) * 1_000_000
    ob["mid"]    = (ob.iloc[:, 4].values + ob.iloc[:, 4+2*N_LVLS].values) / 2.0 / 1e8
    return ob.groupby("ts_sec")["mid"].last().reset_index()


# ── 批量查询工具 ──────────────────────────────────────────────────────────

def _prev_idx(sorted_ts: np.ndarray, query_ts: np.ndarray) -> np.ndarray:
    return np.searchsorted(sorted_ts, query_ts, side="right") - 1


def _grid_val(ts_grid: np.ndarray, mid_grid: np.ndarray,
              query_ts: np.ndarray) -> np.ndarray:
    idx   = _prev_idx(ts_grid, query_ts)
    valid = idx >= 0
    out   = np.full(len(query_ts), np.nan)
    out[valid] = mid_grid[idx[valid]]
    return out


def _rolling_sum(cs: np.ndarray, ts_grid: np.ndarray,
                 query_ts: np.ndarray, window_s: int) -> np.ndarray:
    idx_e = _prev_idx(ts_grid, query_ts)
    idx_b = _prev_idx(ts_grid, query_ts - window_s * 1_000_000 - 1)
    valid = idx_e >= 0
    out   = np.zeros(len(query_ts))
    end_v = np.where(valid, cs[np.clip(idx_e, 0, len(cs)-1)], 0.0)
    beg_v = np.where(idx_b >= 0, cs[np.clip(idx_b, 0, len(cs)-1)], 0.0)
    out[valid] = end_v[valid] - beg_v[valid]
    return out


# ── 核心：单 (symbol, date) 计算 ────────────────────────────────────────

def _compute_day(okx_paths: list[Path], bn_paths: list[Path],
                 target_date: str) -> pd.DataFrame | None:
    """加载 warmup+target 天数据，返回 target_date 当天的特征 DataFrame。"""
    import calendar
    target_sec_lo = calendar.timegm(datetime.strptime(target_date, "%Y%m%d").timetuple()) * 1_000_000
    target_sec_hi = target_sec_lo + 86400 * 1_000_000  # exclusive

    # ── 拼接 OKX 数据 ─────────────────────────────────────────────────
    grids_raw: list[pd.DataFrame] = []
    ob_days:   list[dict]         = []

    for p in sorted(okx_paths):
        df = _load_pq(p)
        if df is None or df.empty:
            continue
        ob = _ob_arrays(df)
        g  = _grid_raw(df)
        del df
        if ob is None or g.empty:
            continue

        ts_sec_arr = g["ts_sec"].values
        ob_idx = _prev_idx(ob["ts"], ts_sec_arr)
        valid  = ob_idx >= 0
        ci     = ob_idx.clip(0)

        bp0_ = np.where(valid, ob["bp0"][ci], np.nan)
        ap0_ = np.where(valid, ob["ap0"][ci], np.nan)
        mid_ = (bp0_ + ap0_) / 2.0
        ba_  = ob["ba"][ci]; aa_ = ob["aa"][ci]
        bid1 = ba_[:, 0]; ask1 = aa_[:, 0]
        bid5 = ba_.sum(1); ask5 = aa_.sum(1)
        d1 = bid1 + ask1; d5 = bid5 + ask5

        g = g.copy()
        g["imb1"]   = np.where(valid & (d1 > 0), (bid1-ask1)/d1, 0.0)
        g["imb5"]   = np.where(valid & (d5 > 0), (bid5-ask5)/d5, 0.0)
        g["spread"] = np.where(valid & (mid_ > 0), (ap0_-bp0_)/mid_*1e4, 0.0)
        del ob
        grids_raw.append(g)

    if not grids_raw:
        return None

    # ── BN 1s-grid 中间价 ─────────────────────────────────────────────
    bn_mids: list[pd.DataFrame] = []
    for p in sorted(bn_paths):
        df = _load_pq(p)
        if df is None or df.empty:
            continue
        gm = _grid_mid_only(df)
        del df
        if not gm.empty:
            bn_mids.append(gm)

    # ── 合并全部天数据，构建 cumsum ───────────────────────────────────
    grid = pd.concat(grids_raw, ignore_index=True).sort_values("ts_sec").reset_index(drop=True)
    grid = grid.groupby("ts_sec", as_index=False).last()  # 去重

    mid_arr   = grid["mid"].values.astype(float)
    lr        = np.zeros(len(mid_arr))
    lr[1:]    = np.log(np.where(mid_arr[:-1] > 0, mid_arr[1:] / mid_arr[:-1], 1.0))

    ts_arr    = grid["ts_sec"].values
    cs_svol   = np.cumsum(grid["signed_vol"].values)
    cs_avol   = np.cumsum(grid["abs_vol"].values)
    cs_lr2    = np.cumsum(lr ** 2)
    imb1_arr  = grid["imb1"].values if "imb1"   in grid.columns else np.zeros(len(ts_arr))
    imb5_arr  = grid["imb5"].values if "imb5"   in grid.columns else np.zeros(len(ts_arr))
    spr_arr   = grid["spread"].values if "spread" in grid.columns else np.zeros(len(ts_arr))

    # ── pdiff cumsum（OKX mid - BN mid）/ BN mid * 1e4 ───────────────
    pdiff_arr  = np.zeros(len(ts_arr))
    pdiff_cnt  = np.zeros(len(ts_arr))
    if bn_mids:
        bn_grid = pd.concat(bn_mids, ignore_index=True).sort_values("ts_sec")
        bn_grid = bn_grid.groupby("ts_sec")["mid"].last().reset_index()
        bn_ts   = bn_grid["ts_sec"].values
        bn_mid  = bn_grid["mid"].values.astype(float)

        bi = _prev_idx(bn_ts, ts_arr)
        bv = bi >= 0
        bc = bi.clip(0)
        bn_m = np.where(bv, bn_mid[bc], np.nan)

        safe = bv & (bn_m > 0) & np.isfinite(mid_arr) & (mid_arr > 0)
        pdiff_arr = np.where(safe, (mid_arr - bn_m) / bn_m * 1e4, 0.0)
        pdiff_cnt = safe.astype(float)

    cs_pdiff = np.cumsum(pdiff_arr)
    cs_pcnt  = np.cumsum(pdiff_cnt)

    # ── 只对 target_date 的 1s 时间点计算特征 ─────────────────────────
    day_mask = (ts_arr >= target_sec_lo) & (ts_arr < target_sec_hi)
    if not day_mask.any():
        return None

    day_ts = ts_arr[day_mask]

    svol60  = _rolling_sum(cs_svol, ts_arr, day_ts,  60)
    avol60  = _rolling_sum(cs_avol, ts_arr, day_ts,  60)
    svol10  = _rolling_sum(cs_svol, ts_arr, day_ts,  10)
    avol10  = _rolling_sum(cs_avol, ts_arr, day_ts,  10)
    lr2_300 = _rolling_sum(cs_lr2,  ts_arr, day_ts, 300)

    mid_now = mid_arr[day_mask]
    mid_60  = _grid_val(ts_arr, mid_arr, day_ts - 60  * 1_000_000)
    mid_300 = _grid_val(ts_arr, mid_arr, day_ts - 300 * 1_000_000)

    svol_ratio = np.where(avol60 > 1e-9, svol60 / avol60, 0.0)
    vpin       = np.where(avol10 > 1e-9, np.abs(svol10) / avol10, -1.0)
    trend60    = np.where(mid_now > 0, (mid_now - mid_60)  / mid_now * 1e4, 0.0)
    trend300   = np.where(mid_now > 0, (mid_now - mid_300) / mid_now * 1e4, 0.0)
    rv_300s    = np.sqrt(np.maximum(lr2_300, 0.0)) * 1e4

    # pdiff rolling mean (2h = 7200s, 12h = 43200s)
    def _pdiff_mean(window_s: int) -> np.ndarray:
        s_e = _rolling_sum(cs_pdiff, ts_arr, day_ts, window_s)
        n_e = _rolling_sum(cs_pcnt,  ts_arr, day_ts, window_s)
        return np.where(n_e > 0, s_e / n_e, 0.0)

    pm_2h  = _pdiff_mean(7200)
    pm_12h = _pdiff_mean(43200)

    return pd.DataFrame({
        "ts_us":     day_ts.astype(np.int64),
        "f0":        imb1_arr[day_mask].astype(np.float64),
        "f1":        imb5_arr[day_mask].astype(np.float64),
        "f2":        spr_arr[day_mask].astype(np.float64),
        "f3":        svol_ratio.astype(np.float64),
        "f4":        vpin.astype(np.float64),
        "f5":        trend60.astype(np.float64),
        "f6":        trend300.astype(np.float64),
        "f7":        rv_300s.astype(np.float64),
        "f8":        pm_2h.astype(np.float64),    # pdiff rolling mean 2h (bps); C++ subtracts realtime pdiff
        "f9":        pm_12h.astype(np.float64),   # pdiff rolling mean 12h (bps)
        "mid_price": mid_now.astype(np.float64),
    })


# ── 进程池 worker ─────────────────────────────────────────────────────────

def _day_task(args: tuple) -> dict:
    symbol, target_date, okx_paths_s, bn_paths_s, out_path_s = args
    okx_paths = [Path(p) for p in okx_paths_s]
    bn_paths  = [Path(p) for p in  bn_paths_s]
    out_path  = Path(out_path_s)

    try:
        df = _compute_day(okx_paths, bn_paths, target_date)
        if df is None or df.empty:
            return {"symbol": symbol, "date": target_date, "rows": 0, "error": "empty"}
        out_path.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(pa.Table.from_pandas(df, preserve_index=False), str(out_path))
        return {"symbol": symbol, "date": target_date, "rows": len(df), "error": None}
    except Exception as e:
        return {"symbol": symbol, "date": target_date, "rows": 0, "error": str(e)}


# ── Pipeline 模块 ─────────────────────────────────────────────────────────

class BuildTickFeatModule(BaseModule):
    """按 (symbol, date) 并发计算 1s tick 特征，加载前一天 warmup 数据确保 rolling 正确。"""

    def run(self, context: Context):
        if not context.sleep(0):
            return None

        cfg      = context.get("cfg") or load_config()
        tick_cfg = cfg.get("tick_feat", {})

        data_dir   = Path(cfg["data"]["input"])
        symbols    = cfg["data"]["symbols"]
        exchange0  = cfg["cpp"]["ini"]["exchange0"]
        exchange1  = cfg["cpp"]["ini"]["exchange1"]
        output_dir = Path(tick_cfg.get("output_dir", "output/tick_feat"))
        workers    = int(tick_cfg.get("workers", 100))
        warmup_days = 2   # 前 2 天 = 48h，覆盖 12h rolling 有余

        output_dir.mkdir(parents=True, exist_ok=True)

        # 可选日期范围过滤
        cfg_sdate = tick_cfg.get("sdate", "")
        cfg_edate = tick_cfg.get("edate", "")

        # ── 收集所有 (symbol, date) 任务 ─────────────────────────────
        job_args: list[tuple] = []
        for sym in symbols:
            okx_all = {_date_from_path(p): p
                       for p in sorted(data_dir.glob(f"{exchange1}_{sym}_*.parquet"))
                       if _date_from_path(p)}
            bn_all  = {_date_from_path(p): p
                       for p in sorted(data_dir.glob(f"{exchange0}_{sym}_*.parquet"))
                       if _date_from_path(p)}
            if not okx_all:
                logger.warning("[%s] 无 OKX 数据，跳过", sym)
                continue

            dates = sorted(okx_all.keys())
            if cfg_sdate:
                dates = [d for d in dates if d >= cfg_sdate]
            if cfg_edate:
                dates = [d for d in dates if d <= cfg_edate]
            sym_dir = output_dir / sym
            for date in dates:
                out_path = sym_dir / f"{date}.parquet"
                if out_path.exists():
                    continue  # 已生成，跳过

                # warmup: 加载前 warmup_days 天 + 当天
                load_dates = [_date_add(date, -i) for i in range(warmup_days, -1, -1)]
                okx_paths = [str(okx_all[d]) for d in load_dates if d in okx_all]
                bn_paths  = [str(bn_all[d])  for d in load_dates if d in bn_all]
                if not okx_paths:
                    continue

                job_args.append((sym, date, okx_paths, bn_paths, str(out_path)))

        total = len(job_args)
        logger.info("BuildTickFeat: %d (symbol,date) 任务, workers=%d", total, workers)
        if total == 0:
            logger.info("BuildTickFeat: 全部已缓存，跳过")
            context.set("tick_feat_dir", str(output_dir))
            return {"symbols": len(symbols), "tasks": 0, "errors": 0}

        done, errors = 0, 0
        with ProcessPoolExecutor(max_workers=workers) as ex:
            futs = {ex.submit(_day_task, a): (a[0], a[1]) for a in job_args}
            for fut in as_completed(futs):
                sym, date = futs[fut]
                if not context.sleep(0):
                    ex.shutdown(wait=False, cancel_futures=True)
                    return None
                try:
                    r = fut.result()
                except Exception as e:
                    r = {"symbol": sym, "date": date, "rows": 0, "error": str(e)}
                done += 1
                if r["error"]:
                    errors += 1
                    logger.warning("[%s][%s] %s", sym, date, r["error"])
                else:
                    logger.debug("[%s][%s] %d行", sym, date, r["rows"])
                if done % 100 == 0 or done == total:
                    logger.info("进度: %d/%d  errors=%d", done, total, errors)

        # ── 按 symbol 合并为单文件供 FeatureLoad ──────────────────────
        logger.info("合并 per-day parquet → per-symbol...")
        merged = 0
        for sym in symbols:
            sym_dir  = output_dir / sym
            day_pqs  = sorted(sym_dir.glob("*.parquet")) if sym_dir.exists() else []
            if not day_pqs:
                continue
            out_sym  = output_dir / f"{sym}.parquet"
            if out_sym.exists():
                # 检查是否需要更新
                newest = max(p.stat().st_mtime for p in day_pqs)
                if out_sym.stat().st_mtime >= newest:
                    merged += 1
                    continue
            try:
                tables = [pq.read_table(str(p)) for p in day_pqs]
                combined = pa.concat_tables(tables)
                pq.write_table(combined, str(out_sym))
                merged += 1
            except Exception as e:
                logger.error("[%s] 合并失败: %s", sym, e)

        logger.info("BuildTickFeat 完成: tasks=%d  errors=%d  merged=%d", total, errors, merged)
        context.set("tick_feat_dir", str(output_dir))
        return {"tasks": total, "errors": errors, "merged": merged}
