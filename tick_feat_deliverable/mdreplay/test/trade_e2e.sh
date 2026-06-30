#!/usr/bin/env bash
# trade_e2e.sh —— 端到端集成测试:拟真 trade 数据 → mdreplay → gconf v1.2.2 TradeRing(广播环)
# → 另一进程 drain 读回,逐条逐字段核对(含 exch_ns、定点价量+scale、GID、side、派发顺序==归并序)。
#
# booktick_e2e.sh / depth_e2e.sh 验 book 段;本脚本验 trade 走的广播环 TradeRing ——
# 「mdreplay 二进制写出的成交环真能被独立消费者按 gconf v2 TradeRing 契约 drain 读懂、且无损有序」。
# 退出码 0=通过。fixture 条数 << 环容量(4096),realtime=0 全速灌也不绕圈(铁律 3)。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG="/shm_trade_e2e"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG}"' EXIT

die() { echo "trade_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# ── 拟真成交数据(单文件、ts 严格递增 → 归并序 = ts 序;混 SOLUSDT/BNBUSDT、买/卖、不同小数位)──
# 定点口径:scale = 去尾零后的小数位(fixed.hpp::decimals_of),trade 价/量各自单值成组。
mkdir -p "$TMP/data"
cat > "$TMP/data/okx_MIX.trade.csv" <<'CSV'
ts,symbol,side,px,qty
1782204109501000000,SOLUSDT,0,68.84,2.5
1782204109502000000,BNBUSDT,1,600.1,10
1782204109503000000,SOLUSDT,1,68.85,0.125
1782204109504000000,BNBUSDT,0,600.25,3
1782204109505000000,SOLUSDT,0,68.8,100
1782204109506000000,BNBUSDT,1,599.99,0.001
CSV

# ── 跑 mdreplay：trade → TradeRing 广播环 ─────────────────────────────────────────────────
echo "[1/3] mdreplay trade → $SEG"
cat > "$TMP/config.toml" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$TMP/data", kind = "trade" }
output = { path = "$SEG", create = true }
[log]
level = "info"
EOF
"$BIN" --config "$TMP/config.toml" \
  2>&1 | grep -E 'done' || die "mdreplay 回放失败"

# ── 编译独立消费者，按 gconf v2 TradeRing 契约 drain 读回 ─────────────────────────────────
echo "[2/3] 编译消费者(独立按 v2 TradeRing 契约 drain)"
cat > "$TMP/reader.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <gconf/shm/v2/trade.h>
namespace v2 = gconf::shm::v2;

// 期望(顺序 = ts 升序 = 归并序):exch_ns, px@scale, qty@scale, gid, side
struct Exp { std::int64_t ns; std::uint32_t px, qty; std::uint16_t gid; std::uint8_t pscale, qscale, side; };
static const Exp kExp[] = {
  {1782204109501000000, 6884,  25,  19, 2, 1, 0},  // SOLUSDT(gid19) buy  68.84/2.5
  {1782204109502000000, 6001,  10,   7, 1, 0, 1},  // BNBUSDT(gid7)  sell 600.1/10
  {1782204109503000000, 6885, 125,  19, 2, 3, 1},  // SOLUSDT sell 68.85/0.125
  {1782204109504000000,60025,   3,   7, 2, 0, 0},  // BNBUSDT buy  600.25/3
  {1782204109505000000,  688, 100,  19, 1, 0, 0},  // SOLUSDT buy  68.8/100
  {1782204109506000000,59999,   1,   7, 2, 3, 1},  // BNBUSDT sell 599.99/0.001
};
static constexpr std::size_t kN = sizeof(kExp) / sizeof(kExp[0]);

int main(int argc, char** argv) {
  int fd = ::shm_open(argv[1], O_RDONLY, 0);
  if (fd < 0) { std::perror("shm_open"); return 2; }
  auto* ring = reinterpret_cast<v2::TradeRing*>(
      ::mmap(nullptr, sizeof(v2::TradeRing), PROT_READ, MAP_SHARED, fd, 0));
  if (ring == MAP_FAILED) { std::perror("mmap"); return 2; }

  std::vector<v2::TradePayload> got;
  std::uint64_t tail = 0;
  std::uint64_t lapped = ring->drain(tail, [&](const v2::TradePayload& p) { got.push_back(p); });

  std::printf("  drained=%zu lapped=%lu (期望 %zu 条)\n", got.size(), (unsigned long)lapped, kN);
  if (lapped) { std::printf("    \xe2\x9d\x8c 被绕圈丢 %lu 条\n", (unsigned long)lapped); return 1; }
  if (got.size() != kN) { std::printf("    \xe2\x9d\x8c 条数不符\n"); return 1; }

  int rc = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    const auto& g = got[i]; const auto& e = kExp[i];
    bool ok = g.exch_ns == e.ns && g.px == e.px && g.qty == e.qty &&
              g.global_symbol_id == e.gid && g.side == e.side &&
              g.price_scale == e.pscale && g.qty_scale == e.qscale;
    std::printf("  [%zu] ns=%ld px=%u@%u qty=%u@%u gid=%u side=%u  %s\n", i,
                (long)g.exch_ns, g.px, g.price_scale, g.qty, g.qty_scale,
                g.global_symbol_id, g.side, ok ? "ok" : "\xe2\x9d\x8c MISMATCH");
    if (!ok) rc = 1;
  }
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt || die "消费者编译失败"

echo "[3/3] 跨进程 drain 读回 + 逐条核对"
if "$TMP/reader" "$SEG"; then
  echo "✅ TradeRing 端到端一致(逐条 exch_ns + 定点价量&scale + GID + side + 派发顺序==归并序 全对)"
else
  die "❌ 读回核对不一致"
fi
