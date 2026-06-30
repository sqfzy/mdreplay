#!/usr/bin/env bash
# multi_e2e.sh —— 端到端集成测试:单进程多路同步回放(一个 config 两个 [[replays]])。
# book + trade 两路在**一个进程、一条全局时钟**下回放,各写各段(book→BookTickBoard、trade→TradeRing);
# 另一进程同时读回两个段核对 —— 证明「单进程多路 + 全局归并按 unit 路由 + 各自契约」整条链路。退出码 0=通过。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG_BK="/shm_multi_book"
readonly SEG_TR="/shm_multi_trade"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG_BK}" "/dev/shm${SEG_TR}"' EXIT

die() { echo "multi_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# ── 拟真两路数据:SOLUSDT 的 book(BBO)与 trade,ts 交错(验全局归并把两路并成一条流)──────────
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
1782204109751000000,SOLUSDT,0,68.91,3
CSV

# ── 一个 config 两路:book→BookTickBoard、trade→TradeRing;共享 realtime=0 一条时钟 ───────────
cat > "$TMP/config.toml" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "book" }
output = { path = "$SEG_BK", create = true }
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "trade" }
output = { path = "$SEG_TR", create = true }
[log]
level = "info"
EOF

echo "[1/3] mdreplay 单进程两路 → $SEG_BK + $SEG_TR"
"$BIN" --config "$TMP/config.toml" 2>&1 | grep -E 'replay#|done' \
  || die "mdreplay 回放失败"

# ── 编译独立消费者,同时按 v1.2.2 契约读回两个段 ───────────────────────────────────────────
echo "[2/3] 编译消费者(独立读 book 段 + trade 段)"
cat > "$TMP/reader.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <gconf/shm/v2/booktick_board.h>
#include <gconf/shm/v2/trade.h>
namespace v2 = gconf::shm::v2;
template <class T> static T* map_ro(const char* name) {
  int fd = ::shm_open(name, O_RDONLY, 0);
  if (fd < 0) { std::perror("shm_open"); return nullptr; }
  auto* p = reinterpret_cast<T*>(::mmap(nullptr, sizeof(T), PROT_READ, MAP_SHARED, fd, 0));
  return p == MAP_FAILED ? nullptr : p;
}
int main(int argc, char** argv) {
  auto* board = map_ro<v2::BookTickBoard>(argv[1]);
  auto* ring  = map_ro<v2::TradeRing>(argv[2]);
  if (!board || !ring) return 2;
  int rc = 0;

  // book:SOLUSDT(lid 19)最新 = uid101,bid 68.85/ask 68.86 @scale2(trade 行没串进 book 段)
  v2::BookTickBoardSlot s; bool ok = board->slot[19].read(s);
  std::printf("  book  lid=19 update_id=%lu bid_px=%u ask_px=%u\n",
              (unsigned long)s.update_id, s.bid_px, s.ask_px);
  if (!ok || s.update_id != 101 || s.bid_px != 6885 || s.ask_px != 6886) { std::printf("    \xe2\x9d\x8c book MISMATCH\n"); rc = 1; }

  // trade:drain 三条(book 行没串进 trade 段),逐条核对 ts/px/side
  struct Exp { std::int64_t ns; std::uint32_t px; std::uint8_t side; };
  static const Exp kExp[] = {
    {1782204109551000000, 6884, 0}, {1782204109651000000, 6886, 1}, {1782204109751000000, 6891, 0},
  };
  std::vector<v2::TradePayload> got; std::uint64_t tail = 0;
  std::uint64_t lapped = ring->drain(tail, [&](const v2::TradePayload& p){ got.push_back(p); });
  std::printf("  trade drained=%zu lapped=%lu (期望 3)\n", got.size(), (unsigned long)lapped);
  if (lapped || got.size() != 3) { std::printf("    \xe2\x9d\x8c trade 条数不符\n"); return 1; }
  for (std::size_t i = 0; i < 3; ++i) {
    const auto& g = got[i]; const auto& e = kExp[i];
    bool tok = g.exch_ns == e.ns && g.px == e.px && g.side == e.side && g.global_symbol_id == 19;
    std::printf("    trade[%zu] ns=%ld px=%u side=%u %s\n", i, (long)g.exch_ns, g.px, g.side, tok ? "ok" : "\xe2\x9d\x8c");
    if (!tok) rc = 1;
  }
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt || die "消费者编译失败"

echo "[3/3] 跨进程同时读回两段 + 核对路由"
if "$TMP/reader" "$SEG_BK" "$SEG_TR"; then
  echo "✅ 单进程多路一致(book 与 trade 各入各段、各自契约、按 unit 路由无串档 全对)"
else
  die "❌ 多路读回核对不一致"
fi
