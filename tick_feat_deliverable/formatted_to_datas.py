#!/usr/bin/env python3
"""formatted_to_datas.py —— tick_feat 侧一次性导出器(在 mdreplay 之外,保其独立)。

把 formatted_2h 的 64 列私有格式转成 mdreplay 的输入契约,落到 datas/:
    <venue>_<symbol>.book.csv   表头  ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty[,..._1..]
    <venue>_<symbol>.trade.csv  表头  ts,symbol,side,px,qty

update_id(mdreplay book 输入必填):formatted 无交易所真值 seqno → 这里**合成 per-symbol 单调计数器**
    (每文件即每 symbol,按 ts 序 1,2,3... 递增)。严格单调 → mdreplay claim_if_newer 去重恒 0。
    注:非交易所真值,仅为让 book 回放开箱可跑;要真值须改 format_jsonl.py 从 raw WS 带出(另开任务)。

档数:formatted 源仅 15 档(bp0..bp14)→ --depth ∈ {1,5,10,15};mdreplay 引擎支持到 25,但 20/25 无源数据。

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

TRADE_HEADER = ["ts", "symbol", "side", "px", "qty"]


def unscale_1e8(int_str: str) -> str:
    """×1e8 整数字符串 → 十进制字符串(整数移位,精确无浮点)。"""
    n = int(int_str)
    s = f"{n // 100_000_000}.{n % 100_000_000:08d}".rstrip("0").rstrip(".")
    return s or "0"


def book_header(depth: int) -> list[str]:
    """book 表头:update_id 紧随 symbol;level0 不带后缀(= BBO),k≥1 为 bid_px_k 等。depth ∈ {1,5,10,15}。"""
    cols = ["ts", "symbol", "update_id"]
    for k in range(depth):
        s = "" if k == 0 else f"_{k}"
        cols += [f"bid_px{s}", f"bid_qty{s}", f"ask_px{s}", f"ask_qty{s}"]
    return cols


def book_row(row: list[str], ts_ns: int, symbol: str, depth: int, update_id: int) -> list:
    """从 64 列 OB 行取 depth 档(价 ×1e8→十进制,量原值透传);update_id = per-symbol 单调计数器。"""
    out: list = [ts_ns, symbol, update_id]
    for k in range(depth):
        out += [unscale_1e8(row[BP0 + k]), row[BA0 + k],
                unscale_1e8(row[AP0 + k]), row[AA0 + k]]
    return out


def parse_venue_symbol(stem: str) -> tuple[str, str]:
    """'binance_swap_SOLUSDT_20260623' → ('binance', 'SOLUSDT')。"""
    parts = stem.split("_")
    if len(parts) < 4:
        raise ValueError(f"文件名不符 <venue>_swap_<symbol>_<date>: {stem}")
    return parts[0], parts[2]


def convert_file(src: Path, out_dir: Path, dry_run: bool, depth: int = 1,
                 by_venue: bool = False) -> tuple[int, int, int]:
    """转一个源文件 → 一对 book/trade 文件。返回 (book_rows, trade_rows, bad_rows)。

    by_venue=False:  out_dir/<venue>_<symbol>.{book,trade}.csv          (扁平,默认)
    by_venue=True:   out_dir/<venue>/<symbol>.{book,trade}.csv          (分 venue 子目录)
        —— 供 mdreplay 按 venue 分段回放(每 venue 一个 shm DepthBoard,避免同 symbol 跨所覆盖)。
    """
    venue, symbol = parse_venue_symbol(src.stem)
    if by_venue:
        dest_dir = out_dir / venue
        stem = symbol
    else:
        dest_dir = out_dir
        stem = f"{venue}_{symbol}"
    if not dry_run:
        dest_dir.mkdir(parents=True, exist_ok=True)  # 幂等:已存在不报错
    book_path = dest_dir / f"{stem}.book.csv"
    trade_path = dest_dir / f"{stem}.trade.csv"

    book_rows = trade_rows = bad_rows = 0
    book_uid = 0  # per-symbol(每文件)单调计数器 → book update_id;严格递增,claim_if_newer 去重恒 0
    book_f = trade_f = None
    book_w = trade_w = None
    try:
        if not dry_run:
            book_f = book_path.open("w", newline="")
            trade_f = trade_path.open("w", newline="")
            book_w = csv.writer(book_f, lineterminator="\n")
            trade_w = csv.writer(trade_f, lineterminator="\n")
            book_w.writerow(book_header(depth))
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
                        book_uid += 1  # 先自增再写 → update_id 从 1 起严格单调
                        if book_w:
                            book_w.writerow(book_row(row, ts_ns, symbol, depth, book_uid))
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
    ap.add_argument("--depth", type=int, default=1, choices=(1, 5, 10, 15),
                    help="book 档数:1(BBO,默认)/5/10/15。formatted 源仅 15 档,故封顶 15;"
                         "mdreplay 引擎支持到 25,但 20/25 无源数据")
    ap.add_argument("--by-venue", action="store_true",
                    help="按 venue 分子目录(out/<venue>/<symbol>.*.csv),供 mdreplay 每 venue 一个段")
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
            b, t, bad = convert_file(src, out_dir, args.dry_run, args.depth, args.by_venue)
        except ValueError as e:
            print(f"  [skip] {src.name}: {e}", file=sys.stderr)
            continue
        tot_book += b
        tot_trade += t
        tot_bad += bad
        print(f"  [{i:>2}/{len(sources)}] {src.name}: book={b} trade={t}"
              + (f" bad={bad}" if bad else ""))

    out_files = 0 if args.dry_run else len(list(out_dir.rglob("*.csv")))  # rglob:含 by-venue 子目录
    print(f"[convert] done: {len(sources)} 源 → {out_files} 文件, "
          f"book={tot_book} trade={tot_trade} bad={tot_bad}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
