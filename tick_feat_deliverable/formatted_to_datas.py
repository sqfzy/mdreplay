#!/usr/bin/env python3
"""formatted_to_datas.py —— tick_feat 侧一次性导出器(在 mdreplay 之外,保其独立)。

把 formatted_2h 的 64 列私有格式转成 mdreplay 的输入契约,落到 datas/:
    <venue>_<symbol>.book.csv   表头  ts,symbol,bid_px,bid_qty,ask_px,ask_qty
    <venue>_<symbol>.trade.csv  表头  ts,symbol,side,px,qty

换算口径(已对源数据核实):
    ts    epoch µs → ns(×1000)
    price/bp*/ap*   ×1e8 整数 → 十进制(整数移位,零浮点损失)
    amount/ba*/aa*  原值透传
    side  0=buy / 1=sell(原样)
    行类型:price 列(col2)==0 → book 行;>0 → 成交行

健壮/幂等:每次清空重写 datas/;--dry-run 只统计不落盘;坏行计数跳过;逐文件进度。
mdreplay 的 C++ 不依赖、不引用本脚本。

用法:
    python3 formatted_to_datas.py [--src formatted_2h] [--out datas] [--dry-run]
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

# 64 列里的关键列下标(0-indexed),已核实
TS, SIDE, PRICE, AMOUNT = 0, 1, 2, 3
BP0, BA0, AP0, AA0 = 4, 19, 34, 49

BOOK_HEADER = ["ts", "symbol", "bid_px", "bid_qty", "ask_px", "ask_qty"]
TRADE_HEADER = ["ts", "symbol", "side", "px", "qty"]


def unscale_1e8(int_str: str) -> str:
    """×1e8 整数字符串 → 十进制字符串(整数移位,精确无浮点)。"""
    n = int(int_str)
    s = f"{n // 100_000_000}.{n % 100_000_000:08d}".rstrip("0").rstrip(".")
    return s or "0"


def parse_venue_symbol(stem: str) -> tuple[str, str]:
    """'binance_swap_SOLUSDT_20260623' → ('binance', 'SOLUSDT')。"""
    parts = stem.split("_")
    if len(parts) < 4:
        raise ValueError(f"文件名不符 <venue>_swap_<symbol>_<date>: {stem}")
    return parts[0], parts[2]


def convert_file(src: Path, out_dir: Path, dry_run: bool) -> tuple[int, int, int]:
    """转一个源文件 → 一对 book/trade 文件。返回 (book_rows, trade_rows, bad_rows)。"""
    venue, symbol = parse_venue_symbol(src.stem)
    book_path = out_dir / f"{venue}_{symbol}.book.csv"
    trade_path = out_dir / f"{venue}_{symbol}.trade.csv"

    book_rows = trade_rows = bad_rows = 0
    book_f = trade_f = None
    book_w = trade_w = None
    try:
        if not dry_run:
            book_f = book_path.open("w", newline="")
            trade_f = trade_path.open("w", newline="")
            book_w = csv.writer(book_f)
            trade_w = csv.writer(trade_f)
            book_w.writerow(BOOK_HEADER)
            trade_w.writerow(TRADE_HEADER)

        with src.open(newline="") as f:
            reader = csv.reader(f)
            next(reader, None)  # 跳表头
            for row in reader:
                try:
                    if len(row) < 64:
                        bad_rows += 1
                        continue
                    ts_ns = int(row[TS]) * 1000
                    price = float(row[PRICE])
                    if price == 0.0:  # OB 行 → book
                        book_rows += 1
                        if book_w:
                            book_w.writerow([ts_ns, symbol, unscale_1e8(row[BP0]),
                                             row[BA0], unscale_1e8(row[AP0]), row[AA0]])
                    else:  # 成交行 → trade
                        trade_rows += 1
                        if trade_w:
                            trade_w.writerow([ts_ns, symbol, row[SIDE],
                                              unscale_1e8(row[PRICE]), row[AMOUNT]])
                except (ValueError, IndexError):
                    bad_rows += 1
    finally:
        if book_f:
            book_f.close()
        if trade_f:
            trade_f.close()

    return book_rows, trade_rows, bad_rows


def main() -> int:
    ap = argparse.ArgumentParser(description="formatted_2h(64列) → datas/(mdreplay 契约)")
    ap.add_argument("--src", default="formatted_2h", help="源目录(默认 formatted_2h)")
    ap.add_argument("--out", default="datas", help="输出目录(默认 datas)")
    ap.add_argument("--dry-run", action="store_true", help="只统计不落盘")
    args = ap.parse_args()

    src_dir = Path(args.src)
    out_dir = Path(args.out)
    if not src_dir.is_dir():
        print(f"[error] 源目录不存在: {src_dir}", file=sys.stderr)
        return 1

    sources = sorted(src_dir.glob("*.csv"))
    if not sources:
        print(f"[error] {src_dir} 下无 *.csv", file=sys.stderr)
        return 1

    if not args.dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)

    mode = "DRY-RUN" if args.dry_run else "WRITE"
    print(f"[convert] {mode}: {src_dir} → {out_dir}  ({len(sources)} 源文件)")

    tot_book = tot_trade = tot_bad = 0
    for i, src in enumerate(sources, 1):
        try:
            b, t, bad = convert_file(src, out_dir, args.dry_run)
        except ValueError as e:
            print(f"  [skip] {src.name}: {e}", file=sys.stderr)
            continue
        tot_book += b
        tot_trade += t
        tot_bad += bad
        print(f"  [{i:>2}/{len(sources)}] {src.name}: book={b} trade={t}"
              + (f" bad={bad}" if bad else ""))

    out_files = 0 if args.dry_run else len(list(out_dir.glob("*.csv")))
    print(f"[convert] done: {len(sources)} 源 → {out_files} 文件, "
          f"book={tot_book} trade={tot_trade} bad={tot_bad}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
