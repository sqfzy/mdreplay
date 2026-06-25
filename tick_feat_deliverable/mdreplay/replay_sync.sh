#!/usr/bin/env bash
# replay_sync.sh —— 给任意 N 个单入单出回放单元盖上**同一个时钟**(跨进程同钟)。
#
# mdreplay 每次调用 = 一个单入单出单元;anchor 与 book/trade 无关(只认"数据时刻↔墙钟")。
# 本脚本只做三件事:① 算一个共享 anchor ② 贴到每个单元一起拉起 ③ 统一生死(Ctrl-C 一次干净停全部)。
# book+trade 只是 N=2 的特例 —— 写两个 --run 即可,脚本不特判任何 kind。
#
# 用法:
#   replay_sync.sh [共享项] --run "<单元1 参数>" --run "<单元2 参数>" ...
#     共享项(贴到每个单元):--realtime --start --end --output.create --delay --system-ts --data-ts
#     每个 --run 串 = 该单元自己的差异参数(--kind --dir --output.path --output.format ...)
#
# 例:
#   ./replay_sync.sh --realtime 1 \
#     --run "--kind book  --dir formatted_2h --output.path /shm_book" \
#     --run "--kind trade --dir formatted_2h --output.path /shm_trade"
#
# 共享 anchor 怎么来(用户零输入):
#   system_ts = --system-ts,否则 now + --delay(默认 3s);只算一次,N 个单元拿同一个值。
#   data_ts   = --data-ts > --start > 自动(扫所有单元文件首行 ts 取全局最小 → 零 burst、精确对齐)。

set -uo pipefail
shopt -s nullglob

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BIN="$SCRIPT_DIR/build/linux/x86_64/release/mdreplay"

die() { echo "replay_sync: $*" >&2; exit 1; }

usage() {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
}

# ── 参数解析 ────────────────────────────────────────────────────────────────
SPECS=(); SHARED=()
DELAY=3; SYSTEM_TS_ARG=""; DATA_TS_ARG=""; START=""

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --run)           SPECS+=("$2"); shift 2;;
      --realtime)      SHARED+=(--realtime "$2"); shift 2;;
      --start)         START="$2"; SHARED+=(--start "$2"); shift 2;;
      --end)           SHARED+=(--end "$2"); shift 2;;
      --output.create) SHARED+=(--output.create "$2"); shift 2;;
      --delay)         DELAY="$2"; shift 2;;
      --system-ts)     SYSTEM_TS_ARG="$2"; shift 2;;
      --data-ts)       DATA_TS_ARG="$2"; shift 2;;
      -h|--help)       usage;;
      *) die "未知参数: $1(--help 查看用法)";;
    esac
  done
}

# 从一个 --run 串里抠出某 flag 的值(如 spec_field "$spec" --dir)。
spec_field() {
  local -a toks; read -ra toks <<< "$1"
  local i
  for ((i = 0; i < ${#toks[@]}; i++)); do
    [ "${toks[$i]}" = "$2" ] && { echo "${toks[$((i + 1))]:-}"; return; }
  done
}

# 列出一个单元(dir,kind,fmt)对应的输入文件;fmt 空/auto 则 csv+json 都看。
list_files() {
  local dir="$1" kind="$2" fmt="$3" f
  local -a fs=()
  case "$fmt" in
    csv | json) fs=("$dir"/*."$kind"."$fmt");;
    *)          fs=("$dir"/*."$kind".csv "$dir"/*."$kind".json);;
  esac
  for f in "${fs[@]}"; do [ -e "$f" ] && printf '%s\n' "$f"; done
}

# 文件首条数据行的 ts(csv 取第一列;json 取 "ts")。文件按 ts 有序,故首行即该文件最小。
first_ts() {
  case "$1" in
    *.csv)  awk -F, 'NR==2{print $1; exit}' "$1";;
    *.json) sed -n '1s/.*"ts"[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' "$1";;
  esac
}

# 自动 data_ts:扫所有单元所有文件首行 ts,取全局最小(≤ 所有事件 → 零 burst)。
scan_global_min_ts() {
  local min="" spec dir kind fmt f t
  for spec in "${SPECS[@]}"; do
    dir=$(spec_field "$spec" --dir); kind=$(spec_field "$spec" --kind)
    fmt=$(spec_field "$spec" --format)
    while IFS= read -r f; do
      t=$(first_ts "$f"); [ -n "$t" ] || continue
      { [ -z "$min" ] || [ "$t" -lt "$min" ]; } && min="$t"
    done < <(list_files "$dir" "$kind" "$fmt")
  done
  echo "$min"
}

resolve_data_ts() {
  if   [ -n "$DATA_TS_ARG" ]; then echo "$DATA_TS_ARG"
  elif [ -n "$START" ];       then echo "$START"
  else scan_global_min_ts
  fi
}

resolve_system_ts() {
  if [ -n "$SYSTEM_TS_ARG" ]; then echo "$SYSTEM_TS_ARG"
  else echo "$(( ($(date -u +%s) + DELAY) * 1000000000 ))"   # now+delay → epoch ns
  fi
}

# ns 整数 → 人读 UTC;datetime 串原样回显。
human_ts() {
  if [[ "$1" =~ ^[0-9]+$ ]]; then date -u -d "@$(( $1 / 1000000000 ))" '+%Y-%m-%d %H:%M:%S UTC'
  else echo "$1 (UTC)"; fi
}

print_banner() {
  echo "同钟回放 · ${#SPECS[@]} 个单元 · 共享时序锚"
  echo "  data_ts   = $(human_ts "$DATA_TS")"
  echo "  system_ts = $(human_ts "$SYSTEM_TS")  ← 全部单元在此刻同时开播"
  local i spec dir kind fmt n
  for i in "${!SPECS[@]}"; do
    spec="${SPECS[$i]}"
    dir=$(spec_field "$spec" --dir); kind=$(spec_field "$spec" --kind); fmt=$(spec_field "$spec" --format)
    n=$(list_files "$dir" "$kind" "$fmt" | wc -l)
    printf '  [%d:%s] --dir %s → %s (%s 文件)\n' \
      "$((i + 1))" "${kind:-?}" "$dir" "$(spec_field "$spec" --output.path)" "$n"
  done
  echo "  Ctrl-C 一次干净停全部单元"
}

# ── 拉起 + 生死 ────────────────────────────────────────────────────────────
PIDS=(); STOPPING=0

launch() {
  local idx="$1" spec="$2" kind label
  kind=$(spec_field "$spec" --kind); label="$((idx + 1)):${kind:-?}"
  # 共享项 + 共享锚 + 本单元差异;输出加 [n:kind] 前缀(进程替换,$! = mdreplay 本体,便于精准 kill -INT)
  # shellcheck disable=SC2086
  "$BIN" "${SHARED[@]}" --anchor.data_ts "$DATA_TS" --anchor.system_ts "$SYSTEM_TS" $spec \
    > >(sed -u "s/^/[$label] /") 2>&1 &
  PIDS+=("$!")
}

stop_all() {
  [ "$STOPPING" = 1 ] && return; STOPPING=1
  echo "replay_sync: 收到中断,向全部单元发 SIGINT(各自优雅收尾)..." >&2
  kill -INT "${PIDS[@]}" 2>/dev/null
}

# 等一个子进程,吸收"被 trap 打断的 wait(返回 >128 而子进程仍在)"→ 重试,返回其真实退出码。
# 否则 Ctrl-C 时 trap 打断 wait 会把干净退出(0)误报成失败。
wait_one() {
  local pid="$1" st
  while :; do
    wait "$pid"; st=$?
    if [ "$st" -gt 128 ] && kill -0 "$pid" 2>/dev/null; then continue; fi
    return "$st"
  done
}

# ── 主流程 ──────────────────────────────────────────────────────────────────
main() {
  parse_args "$@"
  [ -x "$BIN" ] || die "找不到可执行 $BIN —— 先 (cd mdreplay && xmake build mdreplay)"
  [ "${#SPECS[@]}" -ge 1 ] || die "至少要一个 --run \"<单元参数>\"(--help 查看用法)"
  [[ "$DELAY" =~ ^[0-9]+$ ]] || die "--delay 需非负整数秒(得到 '$DELAY')"

  DATA_TS=$(resolve_data_ts)
  [ -n "$DATA_TS" ] || die "无法自动确定 data_ts(没扫到文件首行 ts);请给 --start 或 --data-ts"
  SYSTEM_TS=$(resolve_system_ts)

  print_banner
  trap stop_all INT TERM
  local i; for i in "${!SPECS[@]}"; do launch "$i" "${SPECS[$i]}"; done

  local pid rc=0
  for pid in "${PIDS[@]}"; do wait_one "$pid" || rc=1; done
  exit "$rc"
}

main "$@"
