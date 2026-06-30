#!/usr/bin/env bash
# repro_e2e.sh —— 端到端守门:可复现(铁律 1)。同一输入、realtime=0 跑两遍,shm 段产物**逐字节一致**。
# 覆盖多路(book + trade 两个 [[replays]]),把「改完务必重跑两遍对比字节」这条人工纪律自动化。
# reshape 引入 unit-major 拼接 + 按 unit 路由后这条更易被破坏,故专设此守门。退出码 0=通过。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG_BK="/shm_repro_book"
readonly SEG_TR="/shm_repro_trade"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG_BK}" "/dev/shm${SEG_TR}"' EXIT

die() { echo "repro_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

mkdir -p "$TMP/data"
cat > "$TMP/data/okx_SOLUSDT.book.csv" <<'CSV'
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty
1782204109501000000,SOLUSDT,100,68.84,10,68.85,20
1782204109601000000,SOLUSDT,101,68.85,15,68.86,25
CSV
cat > "$TMP/data/okx_SOLUSDT.trade.csv" <<'CSV'
ts,symbol,side,px,qty
1782204109551000000,SOLUSDT,0,68.84,2.5
1782204109651000000,SOLUSDT,1,68.86,1.5
CSV
cat > "$TMP/config.toml" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "book" }
output = { path = "$SEG_BK", create = true }
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "trade" }
output = { path = "$SEG_TR", create = true }
[log]
level = "warn"
EOF

run_and_snapshot() {  # $1 = 快照后缀
  "$BIN" --config "$TMP/config.toml" >/dev/null 2>&1 || die "mdreplay 回放失败(第 $1 遍)"
  cp "/dev/shm${SEG_BK}" "$TMP/bk_$1"
  cp "/dev/shm${SEG_TR}" "$TMP/tr_$1"
}

echo "[1/2] 跑两遍(realtime=0,create=true 每遍 O_TRUNC 重建)"
run_and_snapshot 1
run_and_snapshot 2

echo "[2/2] 逐字节比对两遍产物"
if cmp -s "$TMP/bk_1" "$TMP/bk_2" && cmp -s "$TMP/tr_1" "$TMP/tr_2"; then
  echo "✅ 可复现守住(book 段 + trade 段两遍逐字节一致,$(stat -c%s "$TMP/bk_1")B + $(stat -c%s "$TMP/tr_1")B)"
else
  cmp "$TMP/bk_1" "$TMP/bk_2" || true
  cmp "$TMP/tr_1" "$TMP/tr_2" || true
  die "❌ 两遍产物有字节差异 —— 可复现被破坏(查归并序 / 定点编码 / 段头初值)"
fi
