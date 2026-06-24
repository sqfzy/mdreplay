#!/usr/bin/env bash
# verify_2h.sh — 一键端到端对拍: format(OKX+BN) → C++ 批量 → 批量 diff(含 f8/f9)
# 用法: bash verify_2h.sh [DATE]   (DATE 默认 20260623)
set -euo pipefail

DATE="${1:-20260623}"
RAW=raw_2h
FMT=formatted_2h
MINE=mine_2h
BIN=cpp/build/linux/x86_64/release/tick_feat_csv

[ -x "$BIN" ] || { echo "缺 C++ 二进制 $BIN, 先 (cd cpp && xmake build tick_feat_csv)"; exit 1; }
[ -d "$RAW" ] || { echo "缺原始目录 $RAW"; exit 1; }

echo "[1/3] format OKX+BN 全部 symbol → $FMT ..."
uv run --no-project --with pyarrow python3 format_jsonl.py \
  --raw_dir "$RAW" --venue both --out_dir "$FMT" --csv --log-level WARNING

echo "[2/3] C++ 批量算特征 → $MINE ..."
mkdir -p "$MINE"
n=0
for f in "$FMT"/okex_swap_*_"$DATE".csv; do
  sym=$(basename "$f" | sed -E "s/okex_swap_(.+)_${DATE}.csv/\1/")
  "$BIN" --raw_dir "$FMT" --symbol "$sym" --date "$DATE" --warmup_days 0 \
         --out "$MINE/$sym.csv" >/dev/null 2>&1 && n=$((n+1))
done
echo "  生成 $n 个"

echo "[3/3] 批量对拍(python 参照 vs C++) ..."
uv run --no-project --with pandas --with pyarrow python3 batch_verify.py \
  --formatted "$FMT" --mine "$MINE" --date "$DATE"
