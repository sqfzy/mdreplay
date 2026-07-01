#!/usr/bin/env bash
# mem_check.sh —— 内存回归守门:验证流式回放的峰值「已提交堆(RssAnon)」与数据量无关。
#
# 这是项目核心卖点(266MB 输入仅 ~0MB 已提交堆,多天连续回放不 OOM)。谁把"整文件入 vector /
# 攒进容器再回放"加回来,RssAnon 就会随数据量线性涨 —— 本检查会红。纯逻辑单测(xmake run test)
# 测不到进程级内存,故单列此操作脚本(仿 verify_2h.sh)。退出码 0=守住,非 0=回归。
#
# 判据:① 绝对上限(流式实测 ~0-5MB;vector 回归 1× 就到 ~264MB)② 3× 与 1× 基本持平(vector 会差 ~2×)。
set -uo pipefail
shopt -s nullglob

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 脚本在 test/,工程根在上一级
readonly BIN="$SCRIPT_DIR/build/linux/x86_64/release/mdreplay"
readonly DATAS="$SCRIPT_DIR/datas"
readonly CEILING_MB=50   # 绝对上限:超过即疑似整文件入内存
readonly DELTA_MB=20     # 3×−1× 上限:流式应持平,涨这么多即内存随数据量走

die() { echo "mem_check: $*" >&2; exit 1; }

# 跑一次 book 回放(realtime=0 尽快、输出 /dev/null),采峰值 RssAnon(MB)。$1=输入目录
peak_rss_anon_mb() {
  local cfg; cfg="$(mktemp)"
  cat > "$cfg" <<EOF
realtime = 0
[[replays]]
input  = { format = "csv", dir = "$1", kind = "trade" }
output = { path = "/shm_memcheck", create = true }
[log]
level = "error"
EOF
  "$BIN" --config "$cfg" >/dev/null 2>&1 &
  local pid=$! hwm=0 v
  while kill -0 "$pid" 2>/dev/null; do
    v=$(awk '/^RssAnon:/{print $2}' "/proc/$pid/status" 2>/dev/null)
    [ -n "${v:-}" ] && [ "$v" -gt "$hwm" ] && hwm="$v"
    sleep 0.02
  done
  rm -f "$cfg"
  echo "$(( hwm / 1024 ))"
}

main() {
  [ -x "$BIN" ] || die "找不到 $BIN —— 先 (cd mdreplay && xmake build mdreplay)"
  [ -d "$DATAS" ] || die "找不到测试数据目录 $DATAS"
  # 用 trade(无需 update_id 列;流式 Source 与 book 同一套,内存特征一致)。
  local base=("$DATAS"/*.trade.csv)
  [ "${#base[@]}" -ge 1 ] || die "$DATAS 下无 *.trade.csv"

  # 造 3× 数据集:同目录复制 3 份不同前缀 → 总行数与文件数都 3 倍。
  local big; big="$(mktemp -d)"
  trap 'rm -rf "$big"; rm -f /dev/shm/shm_memcheck' EXIT
  local d f
  for d in d1 d2 d3; do
    for f in "${base[@]}"; do cp "$f" "$big/${d}_$(basename "$f")"; done
  done

  echo "采样流式 trade 回放峰值 RssAnon(已提交堆,OOM 相关)..."
  local one three diff
  one=$(peak_rss_anon_mb "$DATAS")
  three=$(peak_rss_anon_mb "$big")
  diff=$(( three > one ? three - one : 0 ))
  printf '  1× datas (%d 文件): %3d MB\n  3× big   (%d 文件): %3d MB  (Δ=%d MB)\n' \
    "${#base[@]}" "$one" "$(( ${#base[@]} * 3 ))" "$three" "$diff"

  local rc=0
  [ "$three" -le "$CEILING_MB" ] || { echo "❌ RssAnon ${three}MB > 上限 ${CEILING_MB}MB —— 疑似整文件入内存回归"; rc=1; }
  [ "$diff" -le "$DELTA_MB" ]    || { echo "❌ 3× 比 1× 多 ${diff}MB > ${DELTA_MB}MB —— 内存随数据量涨,流式被破坏"; rc=1; }
  [ "$rc" = 0 ] && echo "✅ 已提交堆与数据量无关(流式守住,多天连续回放不 OOM)"
  exit "$rc"
}

main "$@"
