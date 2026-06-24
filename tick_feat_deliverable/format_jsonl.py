#!/usr/bin/env python3
"""
format_jsonl.py — OKX/Binance 原始 WS JSONL → 标准 64 列 parquet(+可选 CSV)

把 collect_okx_raw.py / collect_binance_raw.py 落盘的原始消息转成
build_tick_feat_standalone.py 期望的标准布局(列序两所严格一致):
  col0=ts(us)  col1=side  col2=price(×1e8, OB行=0)  col3=amount
  col4..18=bp0..14(×1e8)   col19..33=ba0..14(量)
  col34..48=ap0..14(×1e8)  col49..63=aa0..14(量)

venue 差异(仅 build_rows_* 不同, schema/输出共享):
  okx     : books 400档增量 → 本地重建订单簿; trades side=buy/sell; jsonl 前缀 okx_,     输出 okex_swap_
  binance : depth20 全量快照(b/a) 直取前15; trade m=isBuyerMaker; jsonl 前缀 binance_, 输出 binance_swap_

用法:
  uv run --with pyarrow python3 format_jsonl.py --raw_dir raw_2h --venue both --out_dir formatted_2h --csv
  uv run --with pyarrow python3 format_jsonl.py --raw_dir raw_2h --venue binance --symbol SOLUSDT --out_dir formatted_2h --csv
"""
from __future__ import annotations

import argparse
import glob
import json
import logging
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

N_LVLS  = 15
PX_SCALE = 100_000_000          # 1e8

VENUES = {                       # venue → (jsonl 前缀, 输出文件前缀)
    "okx":     ("okx",     "okex_swap"),
    "binance": ("binance", "binance_swap"),
}

logger = logging.getLogger("format_jsonl")


# ── 通用: 标准行构造 / schema / 输出 ─────────────────────────────────────

def _ob_row(ts_us: int, bids: list, asks: list) -> list:
    """OB 行: price=0 标记, 前15档价×1e8 / 量原值, 不足补0; bids 须降序, asks 升序。"""
    row = [ts_us, 0, 0, 0.0]
    bp = [0] * N_LVLS; ba = [0.0] * N_LVLS; ap = [0] * N_LVLS; aa = [0.0] * N_LVLS
    for i, (price, size) in enumerate(bids[:N_LVLS]):
        bp[i] = int(np.rint(price * PX_SCALE)); ba[i] = size
    for i, (price, size) in enumerate(asks[:N_LVLS]):
        ap[i] = int(np.rint(price * PX_SCALE)); aa[i] = size
    return row + bp + ba + ap + aa


def _trade_row(ts_us: int, side: int, price: float, amount: float) -> list:
    return [ts_us, side, int(np.rint(price * PX_SCALE)), amount] + [0] * (4 * N_LVLS)


def _column_names() -> list[str]:
    cols = ["ts", "side", "price", "amount"]
    cols += [f"bp{i}" for i in range(N_LVLS)] + [f"ba{i}" for i in range(N_LVLS)]
    cols += [f"ap{i}" for i in range(N_LVLS)] + [f"aa{i}" for i in range(N_LVLS)]
    return cols


def to_table(rows: list[list]) -> pa.Table:
    """行 → Arrow Table(价/ts/side int64, 量 float64), 按 ts 稳定升序。"""
    rows = sorted(rows, key=lambda r: r[0])
    cols = _column_names()
    columns = list(zip(*rows)) if rows else [[] for _ in cols]
    fields = {}
    for idx, name in enumerate(cols):
        is_float = (name == "amount") or name.startswith("ba") or name.startswith("aa")
        fields[name] = pa.array(list(columns[idx]), type=pa.float64() if is_float else pa.int64())
    return pa.table(fields)


def utc_date_of(table: pa.Table) -> str:
    first_ts_us = table.column("ts")[0].as_py()
    return datetime.fromtimestamp(first_ts_us / 1e6, tz=timezone.utc).strftime("%Y%m%d")


def _iter_msgs(path: Path):
    """逐行读 jsonl, 容忍尾部截断/坏行(便于读正在写入的文件)。"""
    bad = 0
    with path.open() as fh:
        for line in fh:
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                bad += 1
    if bad:
        logger.debug("%s: 跳过 %d 坏行", path.name, bad)


# ── OKX: books 增量重建 ──────────────────────────────────────────────────

class OrderBook:
    def __init__(self):
        self.bids: dict[float, float] = {}
        self.asks: dict[float, float] = {}
        self.ready = False

    def apply(self, entry: dict, action: str) -> None:
        if action == "snapshot" or not self.ready:
            self.bids.clear(); self.asks.clear(); self.ready = True
        self._apply_side(self.bids, entry.get("bids", []))
        self._apply_side(self.asks, entry.get("asks", []))

    @staticmethod
    def _apply_side(book: dict[float, float], levels: list) -> None:
        for level in levels:
            price = float(level[0]); size = float(level[1])
            if size == 0.0: book.pop(price, None)
            else:           book[price] = size

    def top(self, n: int):
        bids = sorted(self.bids.items(), key=lambda kv: -kv[0])[:n]
        asks = sorted(self.asks.items(), key=lambda kv:  kv[0])[:n]
        return bids, asks


def build_rows_okx(files: list[Path]) -> list[list]:
    book = OrderBook()
    rows: list[list] = []
    skipped = 0
    for path in files:
        for msg in _iter_msgs(path):
            m = msg.get("msg", {})
            channel = m.get("arg", {}).get("channel")
            if channel == "books":
                action = m.get("action")
                if not book.ready and action != "snapshot":
                    skipped += 1; continue
                for entry in m.get("data", []):
                    ts_us = int(entry["ts"]) * 1000
                    book.apply(entry, action)
                    bids, asks = book.top(N_LVLS)
                    rows.append(_ob_row(ts_us, bids, asks))
            elif channel == "trades":
                for entry in m.get("data", []):
                    side = 0 if entry["side"] == "buy" else 1
                    rows.append(_trade_row(int(entry["ts"]) * 1000, side, float(entry["px"]), float(entry["sz"])))
    if skipped:
        logger.warning("snapshot 前丢弃 %d 条 book update", skipped)
    return rows


# ── Binance: depth20 全量快照(已排序) ────────────────────────────────────

def build_rows_binance(files: list[Path]) -> list[list]:
    rows: list[list] = []
    for path in files:
        for msg in _iter_msgs(path):
            stream = msg.get("stream", ""); data = msg.get("data")
            if data is None:
                continue
            if "@depth" in stream:
                ts_us = int(data["T"]) * 1000           # 撮合引擎时间(ms)
                bids = [(float(p), float(q)) for p, q in data["b"]]   # 已降序
                asks = [(float(p), float(q)) for p, q in data["a"]]   # 已升序
                rows.append(_ob_row(ts_us, bids, asks))
            elif stream.endswith("@trade"):
                side = 1 if data["m"] else 0            # m=isBuyerMaker → True=主动卖
                rows.append(_trade_row(int(data["T"]) * 1000, side, float(data["p"]), float(data["q"])))
    return rows


BUILDERS = {"okx": build_rows_okx, "binance": build_rows_binance}


# ── 编排 ──────────────────────────────────────────────────────────────────

def _symbol_files(raw_dir: Path, jsonl_prefix: str, symbol: str) -> list[Path]:
    return sorted(Path(p) for p in glob.glob(str(raw_dir / f"{jsonl_prefix}_{symbol}_*.jsonl")))


def _discover_symbols(raw_dir: Path, jsonl_prefix: str) -> list[str]:
    files = glob.glob(str(raw_dir / f"{jsonl_prefix}_*.jsonl"))
    return sorted({Path(p).name[len(jsonl_prefix) + 1:].rsplit("_", 1)[0] for p in files})


def process(raw_dir: Path, venue: str, symbol: str, out_dir: Path, want_csv: bool) -> int:
    jsonl_prefix, out_prefix = VENUES[venue]
    files = _symbol_files(raw_dir, jsonl_prefix, symbol)
    if not files:
        logger.warning("[%s/%s] 无 jsonl", venue, symbol); return 0
    rows = BUILDERS[venue](files)
    if not rows:
        logger.warning("[%s/%s] 无有效行", venue, symbol); return 0
    table = to_table(rows)
    date = utc_date_of(table)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_pq = out_dir / f"{out_prefix}_{symbol}_{date}.parquet"
    pq.write_table(table, str(out_pq))
    logger.info("[%s/%s] %d 行 -> %s", venue, symbol, table.num_rows, out_pq.name)
    if want_csv:
        import pyarrow.csv as pacsv
        pacsv.write_csv(table, str(out_dir / f"{out_prefix}_{symbol}_{date}.csv"))
    return table.num_rows


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="OKX/Binance 原始 JSONL → 标准 64 列 parquet")
    ap.add_argument("--raw_dir", default="raw_2h")
    ap.add_argument("--venue", choices=["okx", "binance", "both"], default="both")
    ap.add_argument("--symbol", help="单个 symbol; 省略则该 venue 全部")
    ap.add_argument("--out_dir", default="formatted_2h")
    ap.add_argument("--csv", action="store_true", help="同时导出 CSV(喂 C++)")
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args(argv)
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(levelname)s %(message)s")

    raw_dir = Path(a.raw_dir)
    if not raw_dir.exists():
        logger.error("raw_dir 不存在: %s", raw_dir); return 1
    venues = ["okx", "binance"] if a.venue == "both" else [a.venue]

    total = 0
    for venue in venues:
        jsonl_prefix = VENUES[venue][0]
        symbols = [a.symbol] if a.symbol else _discover_symbols(raw_dir, jsonl_prefix)
        if not symbols:
            logger.warning("[%s] 未发现 symbol", venue); continue
        for symbol in symbols:
            total += process(raw_dir, venue, symbol, Path(a.out_dir), a.csv)
    logger.info("完成: 合计 %d 行", total)
    return 0 if total > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
