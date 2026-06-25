#!/usr/bin/env bash
# booktick_e2e.sh —— 端到端集成测试:拟真 book 数据 → mdreplay → gconf v1.2.2 BookTickBoard shm 段
# → 另一进程按 v1.2.2 契约读回,逐字段核对(含 update_id 真值、LID 索引、claim_if_newer 去重)。
#
# 纯逻辑单测(xmake run test)在进程内验 BookSink→Board→read;本脚本验「mdreplay 二进制写出的 shm 段
# 真能被独立消费者按 v1.2.2 布局读懂」—— 跨进程契约只有这样才测得到。退出码 0=通过。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG="/shm_booktick_e2e"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG}"' EXIT

die() { echo "booktick_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# ── 拟真 book 数据(带 update_id 列;update_id 倒退的行应被 claim_if_newer 拒)──────────────
mkdir -p "$TMP/data"
cat > "$TMP/data/okx_SOLUSDT.book.csv" <<'CSV'
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty
1782204109501000000,SOLUSDT,100,68.84,1906.67,68.85,1260.72
1782204109601000000,SOLUSDT,101,68.85,1900.00,68.86,1200.00
1782204109701000000,SOLUSDT,99,99.99,1,99.99,1
CSV
cat > "$TMP/data/okx_BNBUSDT.book.csv" <<'CSV'
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty
1782204109550000000,BNBUSDT,5000,600.1,10,600.2,12
CSV

# ── 跑 mdreplay：book → BookTickBoard 段 ──────────────────────────────────────────────────
echo "[1/3] mdreplay book → $SEG"
"$BIN" --kind book --dir "$TMP/data" --format csv --output.path "$SEG" --output.create true --realtime 0 \
  2>&1 | grep -E 'done' || die "mdreplay 回放失败"

# ── 编译独立消费者，按 v1.2.2 BookTickBoard 契约读回 ───────────────────────────────────────
echo "[2/3] 编译消费者(独立按 v1.2.2 契约读)"
cat > "$TMP/reader.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <gconf/shm/v2/booktick_board.h>
namespace v2 = gconf::shm::v2;
int main(int argc, char** argv) {
  int fd = ::shm_open(argv[1], O_RDONLY, 0);
  if (fd < 0) { std::perror("shm_open"); return 2; }
  auto* b = reinterpret_cast<v2::BookTickBoard*>(
      ::mmap(nullptr, sizeof(v2::BookTickBoard), PROT_READ, MAP_SHARED, fd, 0));
  int rc = 0;
  auto chk = [&](int lid, const char* nm, std::uint64_t uid, std::uint32_t bid, std::uint32_t ask) {
    v2::BookTickBoardSlot s; bool ok = b->slot[lid].read(s);
    std::printf("  %-8s lid=%2d update_id=%lu bid_px=%u ask_px=%u\n", nm, lid,
                (unsigned long)s.update_id, s.bid_px, s.ask_px);
    if (!ok || s.update_id != uid || s.bid_px != bid || s.ask_px != ask || s.symbol_lid != lid) {
      std::printf("    \xe2\x9d\x8c MISMATCH(\xe6\x9c\x9f\xe6\x9c\x9b uid=%lu bid=%u ask=%u)\n",
                  (unsigned long)uid, bid, ask);
      rc = 1;
    }
  };
  chk(19, "SOLUSDT", 101, 6885, 6886);  // 最新版(uid=99 被 claim_if_newer 拒);68.85/68.86 @scale2
  chk(7,  "BNBUSDT", 5000, 6001, 6002);  // 600.1/600.2 @scale1
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt || die "消费者编译失败"

echo "[3/3] 跨进程读回 + 逐字段核对"
if "$TMP/reader" "$SEG"; then
  echo "✅ BookTickBoard 端到端一致(update_id 真值 + LID 索引 + claim_if_newer 去重 全对)"
else
  die "❌ 读回核对不一致"
fi
