#!/usr/bin/env bash
# depth_e2e.sh —— 端到端集成测试:拟真多档 book 数据 → mdreplay → mdreplay 本地 DepthBoard 多档段
# → 另一进程按 DepthBoard 布局读回,逐档逐字段核对(含 update_id 真值、LID 索引、全档价量、claim_if_newer 去重)。
#
# booktick_e2e.sh 验单档 BBO(BookTickBoard);本脚本验 depth>1 走的多档段 DepthBoard ——
# 「mdreplay 二进制写出的多档段真能被独立消费者按 mdreplay::DepthBoardSlot 布局读懂」。退出码 0=通过。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG="/shm_depth_e2e"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG}"' EXIT

die() { echo "depth_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# ── 拟真 5 档 book 数据(带 update_id;update_id 倒退行应被 claim_if_newer 拒)────────────────
mkdir -p "$TMP/data"
cat > "$TMP/data/okx_SOLUSDT.book.csv" <<'CSV'
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty,bid_px_1,bid_qty_1,ask_px_1,ask_qty_1,bid_px_2,bid_qty_2,ask_px_2,ask_qty_2,bid_px_3,bid_qty_3,ask_px_3,ask_qty_3,bid_px_4,bid_qty_4,ask_px_4,ask_qty_4
1782204109501000000,SOLUSDT,200,68.84,10,68.85,20,68.83,11,68.86,21,68.82,12,68.87,22,68.81,13,68.88,23,68.80,14,68.89,24
1782204109601000000,SOLUSDT,201,68.85,15,68.86,25,68.84,16,68.87,26,68.83,17,68.88,27,68.82,18,68.89,28,68.81,19,68.90,29
1782204109701000000,SOLUSDT,199,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1,99.99,1
CSV

# ── 跑 mdreplay：depth=5 → DepthBoard 多档段 ──────────────────────────────────────────────
echo "[1/3] mdreplay book(5 档) → $SEG"
"$BIN" --kind book --dir "$TMP/data" --format csv --output.path "$SEG" --output.create true --realtime 0 \
  2>&1 | grep -E 'DepthBoard|done' || die "mdreplay 回放失败(或未走 DepthBoard)"

# ── 编译独立消费者，按 mdreplay::DepthBoard 布局读回 ──────────────────────────────────────
echo "[2/3] 编译消费者(独立按 DepthBoard 布局读)"
cat > "$TMP/reader.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include "output/depth_board.h"
using mdreplay::DepthBoard;
using mdreplay::DepthBoardSlot;
int main(int argc, char** argv) {
  int fd = ::shm_open(argv[1], O_RDONLY, 0);
  if (fd < 0) { std::perror("shm_open"); return 2; }
  auto* b = reinterpret_cast<DepthBoard*>(
      ::mmap(nullptr, sizeof(DepthBoard), PROT_READ, MAP_SHARED, fd, 0));
  if (b == MAP_FAILED) { std::perror("mmap"); return 2; }
  DepthBoardSlot s;
  bool ok = b->slot[19].read(s);  // SOLUSDT LID=19
  std::printf("  SOLUSDT lid=%u update_id=%lu depth=%u  L0[bid=%u ask=%u] L4[bid=%u ask=%u]\n",
              s.symbol_lid, (unsigned long)s.update_id, s.depth,
              s.bid_px[0], s.ask_px[0], s.bid_px[4], s.ask_px[4]);
  int rc = 0;
  // 期望:最新版 uid=201(uid=199 被 claim_if_newer 拒);depth=5;价 @scale2;全档逐一核对。
  if (!ok || s.update_id != 201 || s.depth != 5 || s.symbol_lid != 19) rc = 1;
  if (s.bid_px[0] != 6885 || s.ask_px[0] != 6886) rc = 1;  // L0 = 68.85/68.86
  if (s.bid_px[4] != 6881 || s.ask_px[4] != 6890) rc = 1;  // L4 = 68.81/68.90
  if (s.bid_qty[4] != 19 || s.ask_qty[4] != 29) rc = 1;     // L4 量
  if (s.bid_px[5] != 0 || s.bid_px[24] != 0) rc = 1;        // depth 之外恒 0
  if (rc) std::printf("    \xe2\x9d\x8c MISMATCH\n");
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/src" -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt \
  || die "消费者编译失败"

echo "[3/3] 跨进程读回 + 逐档核对"
if "$TMP/reader" "$SEG"; then
  echo "✅ DepthBoard 端到端一致(全 5 档价量 + update_id 真值 + LID 索引 + claim_if_newer 去重 + 尾档归零 全对)"
else
  die "❌ 读回核对不一致"
fi
