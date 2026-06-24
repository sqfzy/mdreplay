"""
format_parquet_single.py
单交易所数据格式化: 将原始 orderbook + trades CSV 转为 C++ 回测引擎可读的 parquet 格式。
每次只处理一个交易所，输出按 {exchange}_{symbol}_{date}.parquet 命名。

exchange 命名规范: binance_swap, binance_spot, okx_swap, okx_spot, bybit_swap, bybit_spot

用法示例:
    mconf = {
        "exchange": "binance_swap",
        "obs_path": "/Volumes/newhomes/NLshare/market_data/book_snapshot_25/binance-futures",
        "trades_path": "/Volumes/newhomes/NLshare/market_data/trades/binance-futures/trades",
        "output": "/Volumes/PS3000/research/single/binance_swap",
        "lvls": list(range(15)),
        "sdate": "20250201",
        "edate": "20250207",
    }
    run(["SOLUSDT"], mconf)
"""

import os
import sys
import numpy as np
import pandas as pd
from datetime import datetime, timedelta
from decimal import Decimal, getcontext, ROUND_HALF_UP

getcontext().prec = 20

PRECISION = 100000000  # 1e8, 价格统一乘以此值转为整数


# ==================== 工具函数 ====================

def multiply_decimal_series(series, multiplier):
    return np.rint(series.values.astype(np.float64) * multiplier).astype(np.int64)


def max_decimal_length(column):
    decimal_lengths = column.apply(lambda x: len(str(x).split('.')[1]) if '.' in str(x) else 0)
    return decimal_lengths.max()



# 交易所名称 -> 文件中标的命名格式
EXCHANGE_SYMBOL_FORMAT = {
    "binance_swap": lambda s: s,                              # SOLUSDT
    "binance_spot": lambda s: s,                              # SOLUSDT
    "okx_swap":    lambda s: s.replace("USDT", "-USDT-SWAP"),# SOL-USDT-SWAP
    "okx_spot":    lambda s: s.replace("USDT", "-USDT"),     # SOL-USDT
    "bybit_swap":   lambda s: s,                              # SOLUSDT
    "bybit_spot":   lambda s: s,                              # SOLUSDT
}


def get_symbol_mark(exchange, symbol):
    fmt = EXCHANGE_SYMBOL_FORMAT.get(exchange)
    if fmt is None:
        raise ValueError(f"未知交易所: '{exchange}', 支持: {list(EXCHANGE_SYMBOL_FORMAT.keys())}")
    return fmt(symbol)


# ==================== 数据读取 ====================

def read_orderbook(file, lvls):
    print(f"读取 orderbook: {file}")
    df = pd.read_csv(file)
    print(f"  len={len(df)}")

    columns_to_check = []
    for lvl in lvls:
        columns_to_check.append(f'asks[{lvl}].price')
        columns_to_check.append(f'bids[{lvl}].price')

    total_rows = len(df)
    for column in columns_to_check:
        nan_count = df[column].isna().sum()
        if nan_count > total_rows / 1000:
            raise ValueError(f"列 '{column}' 含有超过 0.1% 的 NaN 值")
        else:
            df = df.dropna(subset=[column])
    return df


def read_trades(file):
    print(f"读取 trades: {file}")
    df = pd.read_csv(file)
    print(f"  len={len(df)}")
    return df


# ==================== 校验函数 ====================

def _detect_time_unit(ts_series):
    s = ts_series.dropna()
    if len(s) == 0:
        return None
    vmax = int(s.max())
    if vmax > 10**14:
        return 'us'
    elif vmax > 10**11:
        return 'ms'
    else:
        return 's'


def check_day_bounds(df, expected_date_str, ts_col='local_timestamp'):
    if ts_col not in df.columns:
        print(f"检查失败：时间列 '{ts_col}' 不存在")
        return False

    ts_series = df[ts_col].dropna()
    if len(ts_series) == 0:
        print(f"检查失败：时间列 '{ts_col}' 全为空")
        return False

    unit = _detect_time_unit(ts_series)
    if unit is None:
        return False

    dt_index = pd.to_datetime(ts_series.astype(np.int64), unit=unit, utc=True)
    dt_min = dt_index.min()
    dt_max = dt_index.max()

    expected_start = pd.Timestamp(datetime.strptime(expected_date_str, "%Y%m%d"), tz='UTC')
    expected_next = expected_start + pd.Timedelta(days=1)
    expected_23 = expected_start + pd.Timedelta(hours=23)
    expected_start_plus_1h = expected_start + pd.Timedelta(hours=1)

    print(f"  日期检查: {expected_date_str}, 数据范围=[{dt_min}, {dt_max}]")

    if not (dt_min >= expected_start and dt_min < expected_start_plus_1h):
        print(f"  错误: 首条不在当天 00:00~00:59, dt_min={dt_min}")
        return False

    if not (dt_max >= expected_23 and dt_max < expected_next):
        print(f"  错误: 末条不在当天 23:xx, dt_max={dt_max}")
        return False

    print("  时间边界检查通过")
    return True


def check_row_count(merged_df, source_dfs, source_names):
    expected = sum(len(df) for df in source_dfs)
    actual = len(merged_df)
    for name, df in zip(source_names, source_dfs):
        print(f"  {name}: {len(df)} 行")
    print(f"  合计期望={expected}, 实际={actual}")
    if expected != actual:
        raise ValueError(f"行数不一致! 期望={expected}, 实际={actual}")


def check_price_trailing_9(nc):
    cols = [c for c in nc.columns if c.startswith('bp') or c.startswith('ap')]
    for col in cols:
        ser = nc[col].dropna().astype(str).str.strip()
        if ser.empty:
            continue
        bad_mask = (ser.str.len() > 0) & (ser.str[-1] == '9')
        if bad_mask.any():
            first_idx = bad_mask[bad_mask].index[0]
            first_val = ser.loc[first_idx]
            raise ValueError(f"价格精度问题: 列={col}, 行={first_idx}, 值='{first_val}'")
    print("  价格精度检查通过")


# ==================== min_tick 计算 ====================

def calc_min_tick(nob_df, lvls, file):
    lvls = sorted(lvls)
    diffs_list = []
    for i in range(len(lvls) - 1):
        ch = f"bp{lvls[i]}"; cl = f"bp{lvls[i+1]}"
        if ch in nob_df.columns and cl in nob_df.columns:
            arr = (nob_df[ch] - nob_df[cl]).to_numpy()
            mask = arr > 0
            if mask.any():
                diffs_list.append(arr[mask])

        cl_a = f"ap{lvls[i]}"; ch_a = f"ap{lvls[i+1]}"
        if cl_a in nob_df.columns and ch_a in nob_df.columns:
            arr2 = (nob_df[ch_a] - nob_df[cl_a]).to_numpy()
            mask2 = arr2 > 0
            if mask2.any():
                diffs_list.append(arr2[mask2])

    if len(diffs_list) == 0:
        return None

    diffs = np.concatenate(diffs_list)
    if diffs.size == 0:
        return None

    scale = 100_000_000
    ticks = np.rint(diffs * scale).astype(np.int64)

    chunk_size = 10**6
    counts_dict = {}
    for start in range(0, len(ticks), chunk_size):
        chunk = ticks[start: start + chunk_size]
        uniques, counts = np.unique(chunk, return_counts=True)
        for u, c in zip(uniques, counts):
            counts_dict[u] = counts_dict.get(u, 0) + c

    unique_ticks = np.array(list(counts_dict.keys()))
    counts = np.array(list(counts_dict.values()))

    max_count = counts.max()
    candidate_ticks = unique_ticks[counts == max_count]
    mode_tick = int(candidate_ticks.min())

    mode_diff = mode_tick / float(scale)
    total = ticks.size
    mode_pct = int(max_count) / total
    print(f"  min_tick: {mode_tick} (diff={mode_diff:.8f}, pct={mode_pct*100:.2f}%)")
    if mode_pct < 0.15:
        raise ValueError(f"{file} 占比{mode_pct:.4f} < 0.15")

    return mode_tick


# ==================== 主流程 ====================

def run(symbols, mconf):
    exchange = mconf["exchange"]
    obs_path = mconf["obs_path"]
    trades_path = mconf["trades_path"]
    output = mconf["output"]
    lvls = mconf["lvls"]
    sdate = mconf["sdate"]
    edate = mconf["edate"]

    os.makedirs(output, exist_ok=True)

    start_date = datetime.strptime(sdate, "%Y%m%d")
    end_date = datetime.strptime(edate, "%Y%m%d")

    # 构建列名映射
    cols_bids_price = [f"bids[{i}].price" for i in lvls]
    cols_bids_amount = [f"bids[{i}].amount" for i in lvls]
    cols_asks_price = [f"asks[{i}].price" for i in lvls]
    cols_asks_amount = [f"asks[{i}].amount" for i in lvls]

    ncols_bp = [f"bp{i}" for i in lvls]
    ncols_ba = [f"ba{i}" for i in lvls]
    ncols_ap = [f"ap{i}" for i in lvls]
    ncols_aa = [f"aa{i}" for i in lvls]

    for symbol in symbols:
        print(f"\n{'='*60}")
        print(f"处理 {exchange} / {symbol}")
        print(f"{'='*60}")

        current_date = start_date
        min_tick_val = None
        prev_min_tick = None

        while current_date <= end_date:
            date_str = current_date.strftime("%Y%m%d")
            print(f"\n--- {symbol} {date_str} ---")

            out_file = os.path.join(output, f"{exchange}_{symbol}_{date_str}.parquet")

            # 读取 trades
            symbol_as = get_symbol_mark(exchange, symbol)
            trades_file = os.path.join(trades_path, f"{date_str}_{symbol_as}.csv.gz")
            trades = read_trades(trades_file)

            if not check_day_bounds(trades, date_str):
                raise ValueError(f"trades 时间边界检查失败: {trades_file}")

            trades = trades[["timestamp", "side", "price", "amount"]]
            trades["side"] = trades["side"].map({"buy": 0, "sell": 1}).astype(int)
            trades.columns = ["ts", "side", "price", "amount"]

            # 读取 orderbook
            symbol_as = get_symbol_mark(exchange, symbol)
            obs_file = os.path.join(obs_path, f"{date_str}_{symbol_as}.csv.gz")
            nob = read_orderbook(obs_file, lvls)

            if not check_day_bounds(nob, date_str):
                raise ValueError(f"orderbook 时间边界检查失败: {obs_file}")

            nob = nob[["timestamp"] + cols_bids_price + cols_bids_amount + cols_asks_price + cols_asks_amount]
            nob.columns = ["ts"] + ncols_bp + ncols_ba + ncols_ap + ncols_aa

            # 计算 min_tick (在价格转换前, 用原始浮点数)
            min_tick_val = calc_min_tick(nob, lvls, obs_file)

            # 精度检查
            max_prec = max_decimal_length(nob["bp1"])
            if max_prec > PRECISION:
                raise ValueError(f"精度超限: max_decimal_length={max_prec}")

            # 价格转整数
            for i in lvls:
                nob[f"bp{i}"] = multiply_decimal_series(nob[f"bp{i}"], PRECISION)
                nob[f"ap{i}"] = multiply_decimal_series(nob[f"ap{i}"], PRECISION)

            trades["price"] = multiply_decimal_series(trades["price"], PRECISION)
            trades["price"] = trades["price"].astype("int")

            # 按 [ts, side, price] 聚合 trades: 同一时刻同方向同价格的多笔成交合并
            trades_len_before = len(trades)
            trades = trades.groupby(["ts", "side", "price"], as_index=False).agg({"amount": "sum"})
            trades = trades.sort_values("ts").reset_index(drop=True)
            print(f"  trades 聚合: {trades_len_before} -> {len(trades)} (-{trades_len_before - len(trades)})")

            # 合并排序 (单所: trades + orderbook)
            nc = pd.concat([trades, nob]).sort_values("ts")
            nc.fillna(0, inplace=True)

            # 类型转换
            for i in lvls:
                nc[f"ap{i}"] = nc[f"ap{i}"].astype("int")
                nc[f"bp{i}"] = nc[f"bp{i}"].astype("int")
                nc[f"aa{i}"] = nc[f"aa{i}"].astype("float")
                nc[f"ba{i}"] = nc[f"ba{i}"].astype("float")
            nc["price"] = nc["price"].astype("int")
            nc["side"] = nc["side"].astype("int")

            # 校验 (trades 已聚合, 用聚合后的行数)
            check_row_count(nc, [trades, nob], ["trades(aggregated)", "orderbook"])
            check_price_trailing_9(nc)

            # 写出 (带 min_tick metadata)
            import pyarrow as pa
            table = pa.Table.from_pandas(nc, preserve_index=False)
            tick_changed = "false"
            if prev_min_tick is not None and min_tick_val != prev_min_tick:
                tick_changed = "true"
                print(f"  ⚠ tick_changed! {prev_min_tick} -> {min_tick_val}")
            meta = table.schema.metadata or {}
            meta[b"min_tick"] = str(min_tick_val).encode()
            meta[b"tick_changed"] = tick_changed.encode()
            table = table.replace_schema_metadata(meta)
            import pyarrow.parquet as pq
            pq.write_table(table, out_file, compression="snappy")
            print(f"  输出: {out_file} ({len(nc)} 行, min_tick={min_tick_val}, tick_changed={tick_changed})")

            prev_min_tick = min_tick_val
            current_date += timedelta(days=1)

    print(f"\n完成! 输出目录: {output}")


if __name__ == "__main__":
    # 示例: python format_parquet_single.py
    # 需要根据实际路径修改 mconf
    print("请通过 import 调用 run(symbols, mconf), 或修改此处配置后运行")
