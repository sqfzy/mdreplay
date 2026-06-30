#!/usr/bin/env bash
# multi_venue_e2e.sh —— 端到端集成测试:**同 kind 多 venue 各写各段**(reshape 头号卖点)+ per-replay 时间窗。
# 两路都是 book(venue A=SOLUSDT、venue B=BNBUSDT),各写自己的段;B 配本路时间窗排除末行。
# 验证:① 各 venue 数据只落各自段(无串段)② BookTickBoard latest-wins 下 B 的"最新"被窗口裁掉末行。
# 这条恰好补 multi_e2e.sh(book+trade,不同 kind 天然不串段)没覆盖的「同 kind 路由 + 窗口接线」。退出码 0=通过。
set -uo pipefail

readonly DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$DIR/build/linux/x86_64/release/mdreplay"
readonly SEG_A="/shm_venue_a"
readonly SEG_B="/shm_venue_b"
readonly TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "/dev/shm${SEG_A}" "/dev/shm${SEG_B}"' EXIT

die() { echo "multi_venue_e2e: $*" >&2; exit 1; }
[ -x "$BIN" ] || die "找不到 $BIN —— 先 xmake build mdreplay"

# 时间窗用秒级 UTC datetime(parse_datetime_ns 口径)→ 三行落在相邻三秒,便于窗口卡在中间秒。
readonly T0="2026-06-23 09:00:00"
readonly T1="2026-06-23 09:00:01"
readonly T2="2026-06-23 09:00:02"
s2ns() { echo "$(( $(date -u -d "$1" +%s) * 1000000000 ))"; }
NS0=$(s2ns "$T0"); NS1=$(s2ns "$T1"); NS2=$(s2ns "$T2")

mkdir -p "$TMP/a" "$TMP/b"
# venue A:SOLUSDT(LID 19)两行,无窗口 → 段存最新 uid=101
cat > "$TMP/a/okx_SOLUSDT.book.csv" <<CSV
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty
$NS0,SOLUSDT,100,68.84,10,68.85,20
$NS1,SOLUSDT,101,68.85,15,68.86,25
CSV
# venue B:BNBUSDT(LID 7)三行;本路窗口 end=T1 → 排除 T2 末行 → 段存最新 uid=201(不是 202)
cat > "$TMP/b/bn_BNBUSDT.book.csv" <<CSV
ts,symbol,update_id,bid_px,bid_qty,ask_px,ask_qty
$NS0,BNBUSDT,200,600.1,10,600.2,12
$NS1,BNBUSDT,201,600.2,11,600.3,13
$NS2,BNBUSDT,202,600.3,12,600.4,14
CSV

cat > "$TMP/config.toml" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$TMP/a", kind = "book" }
output = { path = "$SEG_A", create = true }
[[replays]]
input  = { format = "csv", dir = "$TMP/b", kind = "book" }
output = { path = "$SEG_B", create = true }
end    = "$T1"
[log]
level = "info"
EOF

echo "[1/3] mdreplay 两路同 kind(book A + book B)→ $SEG_A + $SEG_B"
"$BIN" --config "$TMP/config.toml" 2>&1 | grep -E 'replay#|done' || die "mdreplay 回放失败"

echo "[2/3] 编译消费者(独立读两段)"
cat > "$TMP/reader.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <gconf/shm/v2/booktick_board.h>
namespace v2 = gconf::shm::v2;
static v2::BookTickBoard* map_ro(const char* name) {
  int fd = ::shm_open(name, O_RDONLY, 0);
  if (fd < 0) { std::perror("shm_open"); return nullptr; }
  auto* p = reinterpret_cast<v2::BookTickBoard*>(
      ::mmap(nullptr, sizeof(v2::BookTickBoard), PROT_READ, MAP_SHARED, fd, 0));
  return p == MAP_FAILED ? nullptr : p;
}
static std::uint64_t uid_of(v2::BookTickBoard* b, int lid) {
  v2::BookTickBoardSlot s; (void)b->slot[lid].read(s); return s.update_id;
}
int main(int argc, char** argv) {
  auto* a = map_ro(argv[1]); auto* b = map_ro(argv[2]);
  if (!a || !b) return 2;
  const std::uint64_t a_sol = uid_of(a, 19), a_bnb = uid_of(a, 7);
  const std::uint64_t b_sol = uid_of(b, 19), b_bnb = uid_of(b, 7);
  std::printf("  segA: SOLUSDT uid=%lu  BNBUSDT uid=%lu\n", (unsigned long)a_sol, (unsigned long)a_bnb);
  std::printf("  segB: SOLUSDT uid=%lu  BNBUSDT uid=%lu\n", (unsigned long)b_sol, (unsigned long)b_bnb);
  int rc = 0;
  // A 段只该有 SOLUSDT(uid=101),BNBUSDT 槽空(未串段)
  if (a_sol != 101 || a_bnb != 0) { std::printf("    \xe2\x9d\x8c segA 串段/缺值\n"); rc = 1; }
  // B 段只该有 BNBUSDT,且窗口排除 T2 末行 → uid=201(非 202);SOLUSDT 槽空
  if (b_bnb != 201 || b_sol != 0) { std::printf("    \xe2\x9d\x8c segB 窗口失效/串段\n"); rc = 1; }
  return rc;
}
CPP
g++ -std=c++23 -O2 -I"$DIR/gconf/include" "$TMP/reader.cpp" -o "$TMP/reader" -lrt || die "消费者编译失败"

echo "[3/3] 跨进程读回两段 + 核对路由与窗口"
if "$TMP/reader" "$SEG_A" "$SEG_B"; then
  echo "✅ 同 kind 多 venue 一致(各 venue 只落各自段、无串段 + per-replay 窗口裁掉末行 全对)"
else
  die "❌ 多 venue 读回核对不一致"
fi
