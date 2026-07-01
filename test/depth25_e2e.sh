#!/usr/bin/env bash
# depth25_e2e.sh —— 端到端集成测试:拟真 **25 档** book 数据 → mdreplay → mdreplay 本地 DepthBoard
# → 另一进程按 DepthBoard 布局读回,逐档逐字段核对(含 depth=25、update_id 真值、L0/L12/L24 全档价量、
#   claim_if_newer 去重)。
#
# depth_e2e.sh 验 5 档;本脚本验引擎/段的**最大档数 25**(kMaxDepth)端到端——
# 「mdreplay 写出的 25 档段真能被独立消费者按 mdreplay::DepthBoardSlot 布局逐档读懂」。退出码 0=通过。
# fixture 自包含、确定性(价用整数分生成,严格单调,不依赖 formatted 源数据)。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG="/shm_depth25_e2e"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG}"' EXIT

die() { echo "depth25_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# ── 拟真 25 档 book 数据(整数分生成保严格单调;update_id 倒退行应被 claim_if_newer 拒)──────────
# 价定点 @scale2(组内含 2 位小数 → 公共 price_scale=2);量为整数 @scale0。
#   row uid=300: bid_px[k]=6884-k 分, ask_px[k]=6885+k 分; bid_qty[k]=10+k, ask_qty[k]=20+k
#   row uid=301: bid_px[k]=6885-k 分, ask_px[k]=6886+k 分; bid_qty[k]=15+k, ask_qty[k]=25+k  ← 最新,应入段
#   row uid=299: 全 99.99/1                                                                  ← 倒退,应被去重
mkdir -p "$TMP/data"
awk 'BEGIN{
  OFS=","
  # 表头
  h="ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty"
  for(k=1;k<25;k++) h=h",bid_px_"k",bid_qty_"k",ask_px_"k",ask_qty_"k
  print h
  # cents(c) → "D.CC" 十进制串
  # 三行:(ts, uid, bidbase_cents, askbase_cents, bidqty0, askqty0)
  emit("1782204109501000000",300,6884,6885,10,20)
  emit("1782204109601000000",301,6885,6886,15,25)
  emit("1782204109701000000",299,9999,9999,1,1)   # 倒退;价全平(仅占位,会被去重)
}
function dec(c,  d,r){ d=int(c/100); r=c%100; return d"."sprintf("%02d",r) }
function emit(ts,uid,bb,ab,bq,aq,  line,k){
  line=ts",SOLUSDT,"uid
  for(k=0;k<25;k++){
    if(uid==299){ line=line",99.99,1,99.99,1" }
    else        { line=line","dec(bb-k)","(bq+k)","dec(ab+k)","(aq+k) }
  }
  print line
}' > "$TMP/data/okx_SOLUSDT.book.csv"

# 健全性:表头应为 1(ts)+1(symbol)+1(uid)+25*4 = 103 列
ncols=$(head -1 "$TMP/data/okx_SOLUSDT.book.csv" | awk -F',' '{print NF}')
[ "$ncols" = "103" ] || die "fixture 列数 $ncols != 103(25 档表头生成有误)"

# ── 跑 mdreplay：depth=25 → DepthBoard 多档段 ─────────────────────────────────────────────
echo "[1/3] mdreplay book(25 档) → $SEG"
cat > "$TMP/config.toml" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "book" }
output = { path = "$SEG", create = true }
[log]
level = "info"
EOF
"$BIN" --config "$TMP/config.toml" \
  2>&1 | grep -E 'DepthBoard|done' || die "mdreplay 回放失败(或未走 DepthBoard)"

# ── 编译独立消费者，按 mdreplay::DepthBoard 布局逐档读回 ─────────────────────────────────
echo "[2/3] 编译消费者(独立按 DepthBoard 布局读 25 档)"
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
  std::printf("  SOLUSDT lid=%u update_id=%lu depth=%u pscale=%u qscale=%u\n",
              s.symbol_lid, (unsigned long)s.update_id, s.depth, s.price_scale, s.qty_scale);
  std::printf("    L0 [bid=%u/%u ask=%u/%u] L12 [bid=%u/%u ask=%u/%u] L24 [bid=%u/%u ask=%u/%u]\n",
              s.bid_px[0], s.bid_qty[0], s.ask_px[0], s.ask_qty[0],
              s.bid_px[12], s.bid_qty[12], s.ask_px[12], s.ask_qty[12],
              s.bid_px[24], s.bid_qty[24], s.ask_px[24], s.ask_qty[24]);
  int rc = 0;
  // 期望:uid=301 入段(uid=299 被 claim_if_newer 拒);depth=25;价@scale2、量@scale0;
  //   bid_px[k]=6885-k, ask_px[k]=6886+k(分);bid_qty[k]=15+k, ask_qty[k]=25+k。
  if (!ok || s.update_id != 301 || s.depth != 25 || s.symbol_lid != 19) rc = 1;
  if (s.price_scale != 2 || s.qty_scale != 0) rc = 1;
  if (s.bid_px[0]  != 6885 || s.ask_px[0]  != 6886 || s.bid_qty[0]  != 15 || s.ask_qty[0]  != 25) rc = 1;
  if (s.bid_px[12] != 6873 || s.ask_px[12] != 6898 || s.bid_qty[12] != 27 || s.ask_qty[12] != 37) rc = 1;
  if (s.bid_px[24] != 6861 || s.ask_px[24] != 6910 || s.bid_qty[24] != 39 || s.ask_qty[24] != 49) rc = 1;
  if (rc) std::printf("    \xe2\x9d\x8c MISMATCH\n");
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/src" -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt \
  || die "消费者编译失败"

echo "[3/3] 跨进程读回 + 逐档核对(L0/L12/L24)"
if "$TMP/reader" "$SEG"; then
  echo "✅ DepthBoard 25 档端到端一致(depth=25 + L0/L12/L24 全档价量 + scale + update_id 去重 全对)"
else
  die "❌ 读回核对不一致"
fi
