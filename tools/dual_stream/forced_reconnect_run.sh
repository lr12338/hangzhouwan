#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 板端 forced-reconnect 维护窗口测试运行器（BM1684 VPU 显存重连验证）。
#
# 审计要点（对应维护窗口验证要求）：
#   1. 生产仍运行时拒绝执行（systemctl is-active + pgrep 双重只读检测）。
#   2. 仅使用指定候选 Release 的 bin/forced_reconnect_tool；校验其非 current/previous。
#   3. 每阶段设置 timeout；超时（rc=124）视为失败。
#   4. 任一阶段失败（rc!=0 或超时）立即停止后续阶段（fail-fast）。
#   5. trap INT/TERM 保证写汇总并退出；timeout -k 保证工具进程被回收。
#   6. 不写入 /etc/hangzhouwan、不修改 current/previous 软链接、不 systemctl 修改/重启。
#   7. 输出机器可读 JSON 汇总 + 非零失败码。
#
# 测试矩阵：
#   heap        所有 VPU 堆基线（只读，含 heap0..heapN，非仅 heap2）
#   repro 3     泄漏隔离复现（A_leak 单调增长 / B_safe 持平）
#   decoder 100 decoder-only 强制重连 100 次（extra_frame_buffer_num=20）
#   rtsp   100  RTSP-only 强制重连 100 次（复用 decoder 解码器换建路径，文件源模拟）
#   sweep       extra_frame_buffer_num 5/8/12/20 各 20 次重连
#   dual   100  双路并发强制重连 100 次（快速故障注入）
#   rtmp        RTMP muxer-only 重建决策自检（编码器不可用 => 致命 70）
#   注：RTMP 端到端 100x + 编码器创建计数为运维 Runbook 独立步骤（需 RTMP 接收端）。
#
# 工具按每 10 轮记录快照（start/10/20/.../100/final），覆盖请求的 0/10/50/100；
# 25 与 75 由相邻快照（20/30、70/80）包夹，第 1 轮逐轮结果见原始日志。
#
# 用法：
#   bash tools/dual_stream/forced_reconnect_run.sh [mode] [rounds]
#     mode: all(默认) | heap | repro | decoder | rtsp | rtmp | dual | sweep
#     rounds: 重连次数（decoder/rtsp/dual 默认 100，sweep 每档 20，repro=3）
#
# 环境：
#   HZW_CANDIDATE_RELEASE   候选 Release 目录
#                           （默认 /opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2）
#   HZW_FORCE_RUN=1         跳过生产运行检测（仅维护窗口停产后人工确认用，慎用）
#   HZW_PHASE_TIMEOUT_<NAME> 覆盖阶段超时（NAME=HEAP|REPRO|DECODER|RTSP|SWEEP|DUAL|RTMP）
#
# 前置：候选 Release 已构建；生产已停止（释放 VPU 预算）；testdata/test.mp4 存在。
# 输出：artifacts/internal-development/forced_reconnect_<phase>.{raw,log}
#       artifacts/internal-development/forced_reconnect_summary.json
# =============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

LOG_DIR="artifacts/internal-development"
mkdir -p "$LOG_DIR"
SUMMARY_FILE="$LOG_DIR/forced_reconnect_summary.json"

DEFAULT_RELEASE="/opt/hangzhouwan/releases/vpu-reconnect-fix-1a1a7b2"
RELEASE_DIR="${HZW_CANDIDATE_RELEASE:-$DEFAULT_RELEASE}"
BASE_DIR="/opt/hangzhouwan"
CURRENT_LINK="$BASE_DIR/current"
PREVIOUS_LINK="$BASE_DIR/previous"

export LD_LIBRARY_PATH="/opt/sophon/libsophon-0.4.9/lib:/opt/sophon/sophon-ffmpeg_0.8.0/lib:${LD_LIBRARY_PATH:-}"
TOOL="$RELEASE_DIR/bin/forced_reconnect_tool"
TEST_CLIP="testdata/test.mp4"

MODE="${1:-all}"
ROUNDS="${2:-100}"

OVERALL_RC=0
PHASES_JSON=""
START_TS="$(date -u +%s)"

log() { printf '%s | %s\n' "$(date -u +%FT%TZ)" "$*" >&2; }

# JSON 字符串转义（双引号/反斜杠，去换行）
json_str() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\n\r'
}

phase_timeout() {
  local name="$1" def="$2" v
  v="HZW_PHASE_TIMEOUT_${name}"
  if [ -n "${!v:-}" ]; then echo "${!v}"; else echo "$def"; fi
}

# 从 decoder/rtsp 原始日志提取每 10 轮快照为 JSON 数组（避免子shell，用进程替换）
extract_rounds_json() {
  local raw="$1" out="" line rnd heap delta okc failc inflight
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    rnd="$(printf '%s' "$line" | sed -n 's/.*round \([0-9][0-9]*\).*/\1/p')"
    [ -z "$rnd" ] && continue
    heap="$(printf '%s' "$line" | sed -n 's/.*heap2_used=\([0-9.]*\)MB.*/\1/p')"
    delta="$(printf '%s' "$line" | sed -n 's/.*delta=\([0-9.+-]*\)MB.*/\1/p')"
    delta="${delta#+}"
    okc="$(printf '%s' "$line" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')"
    failc="$(printf '%s' "$line" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')"
    inflight="$(printf '%s' "$line" | sed -n 's/.*inflight=\([0-9]*\).*/\1/p')"
    [ -z "$heap" ] && heap="null"
    [ -z "$delta" ] && delta="null"
    [ -z "$okc" ] && okc="null"
    [ -z "$failc" ] && failc="null"
    [ -z "$inflight" ] && inflight="null"
    out="${out}{\"round\":${rnd},\"heap2_used_mb\":${heap},\"delta_mb\":${delta},\"ok\":${okc},\"fail\":${failc},\"inflight\":${inflight}},"
  done < <(grep -E '\| round [0-9]+ \|' "$raw" 2>/dev/null || true)
  printf '%s' "${out%,}"
}

write_summary() {
  local end_ts dur pj cp d_raw
  end_ts="$(date -u +%s)"
  dur=$((end_ts - START_TS))
  pj="${PHASES_JSON%,}"
  cp=""
  d_raw="$LOG_DIR/forced_reconnect_decoder.raw"
  if [ -f "$d_raw" ]; then cp="$(extract_rounds_json "$d_raw")"; fi
  {
    printf '{\n'
    printf '  "candidate_release": "%s",\n' "$(json_str "$RELEASE_DIR")"
    printf '  "tool": "%s",\n' "$(json_str "$TOOL")"
    printf '  "mode": "%s",\n' "$MODE"
    printf '  "started_at_epoch": %s,\n' "$START_TS"
    printf '  "duration_s": %s,\n' "$dur"
    printf '  "overall_rc": %s,\n' "$OVERALL_RC"
    printf '  "overall": "%s",\n' "$([ "$OVERALL_RC" -eq 0 ] && echo PASS || echo FAIL)"
    printf '  "phases": [%s],\n' "$pj"
    printf '  "decoder_checkpoints": [%s],\n' "$cp"
    printf '  "notes": [\n'
    printf '    "VPU_HEAP 行含所有可用堆(heap0..heapN)，非仅 heap2",\n'
    printf '    "checkpoints 为工具每 10 轮快照(start/10/20/.../100/final)，覆盖 0/10/50/100；25/75 由相邻快照包夹",\n'
    printf '    "RTMP 端到端 100x 编码器创建计数见运维 Runbook(maintenance-window-runbook.md)",\n'
    printf '    "任一阶段 rc!=0 即 fail-fast 停止后续；超时 rc=124"\n'
    printf '  ]\n'
    printf '}\n'
  } >"$SUMMARY_FILE"
  log "机器可读汇总: $SUMMARY_FILE"
}

finalize() {
  write_summary
  log "overall_rc=$OVERALL_RC ($( [ "$OVERALL_RC" -eq 0 ] && echo PASS || echo FAIL ))"
  exit "$OVERALL_RC"
}

on_interrupt() {
  log "收到中断信号，写汇总后退出"
  OVERALL_RC=130
  finalize
}
trap on_interrupt INT TERM

# ---- 安全门禁：生产运行时拒绝执行 ----
refuse_if_production_running() {
  if [ "${HZW_FORCE_RUN:-0}" = "1" ]; then
    log "警告: HZW_FORCE_RUN=1 已跳过生产运行检测（仅维护窗口停产后使用）"
    return 0
  fi
  local prod_running=0
  if systemctl is-active --quiet hangzhouwan-video.service 2>/dev/null; then
    log "拒绝: hangzhouwan-video.service 仍为 active"; prod_running=1
  fi
  if systemctl is-active --quiet hangzhouwan.target 2>/dev/null; then
    log "拒绝: hangzhouwan.target 仍为 active"; prod_running=1
  fi
  if pgrep -x dual_stream_app >/dev/null 2>&1; then
    log "拒绝: 检测到 dual_stream_app 进程在运行（生产未停止）"; prod_running=1
  fi
  if [ "$prod_running" -ne 0 ]; then
    log "生产仍在运行，VPU 预算未释放。需先在维护窗口执行: sudo systemctl stop hangzhouwan.target"
    log "未获维护窗口批准前不得停止生产。本脚本拒绝执行。"
    return 1
  fi
  log "生产已停止（video/target inactive，无 dual_stream_app 进程）"
  return 0
}

# ---- 候选 Release 校验：仅操作指定候选，不得为 current/previous ----
validate_candidate() {
  if [ ! -d "$RELEASE_DIR" ]; then
    log "拒绝: 候选 Release 目录不存在: $RELEASE_DIR"; return 1
  fi
  if [ ! -x "$TOOL" ]; then
    log "拒绝: 候选 Release 缺少可执行 forced_reconnect_tool: $TOOL"; return 1
  fi
  local cur prev rel_real
  rel_real="$(cd "$RELEASE_DIR" && pwd)"
  cur="$(readlink -f "$CURRENT_LINK" 2>/dev/null || true)"
  prev="$(readlink -f "$PREVIOUS_LINK" 2>/dev/null || true)"
  if [ "$rel_real" = "$cur" ]; then
    log "拒绝: 候选 Release 即当前生产 current，禁止对生产二进制压测: $rel_real"; return 1
  fi
  if [ -n "$prev" ] && [ "$rel_real" = "$prev" ]; then
    log "拒绝: 候选 Release 即生产 previous，禁止压测: $rel_real"; return 1
  fi
  if [ ! -f "$TEST_CLIP" ]; then
    log "拒绝: 缺少测试片源 $TEST_CLIP（工具硬编码相对路径，需在仓库根运行）"; return 1
  fi
  log "候选 Release: $rel_real（非 current/previous）"
  log "工具: $TOOL"
  log "current -> ${cur:-（无）} | previous -> ${prev:-（无）}"
  return 0
}

# ---- 解析单阶段原始日志，追加 JSON 片段 ----
record_phase() {
  local name="$1" rc="$2" tmo="$3" dur="$4" raw="$5"
  local ok="null" fail="null" delta="null" inflight="null" fatal="null"
  local done_line heap_start heap_final frag
  done_line="$(grep -E '\| done \||simulate\(' "$raw" 2>/dev/null | tail -1 || true)"
  case "$name" in
    decoder|rtsp|sweep_*)
      ok="$(printf '%s' "$done_line" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')"
      fail="$(printf '%s' "$done_line" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')"
      delta="$(printf '%s' "$done_line" | sed -n 's/.*delta=\([0-9.+-]*\)MB.*/\1/p')"
      ;;
    dual)
      local af bf
      af="$(printf '%s' "$done_line" | sed -n 's/.*A_fail=\([0-9]*\).*/\1/p')"
      bf="$(printf '%s' "$done_line" | sed -n 's/.*B_fail=\([0-9]*\).*/\1/p')"
      af="${af:-0}"; bf="${bf:-0}"
      fail="$((af + bf))"
      delta="$(printf '%s' "$done_line" | sed -n 's/.*delta=\([0-9.+-]*\)MB.*/\1/p')"
      ;;
    rtmp)
      fatal="$(printf '%s' "$done_line" | sed -n 's/.*fatal=\([0-9]*\).*/\1/p')"
      ;;
  esac
  inflight="$(grep -E '\| round [0-9]+ \|' "$raw" 2>/dev/null | tail -1 | sed -n 's/.*inflight=\([0-9]*\).*/\1/p' || true)"
  delta="${delta#+}"
  [ -z "$ok" ] && ok="null"
  [ -z "$fail" ] && fail="null"
  [ -z "$delta" ] && delta="null"
  [ -z "$inflight" ] && inflight="null"
  [ -z "$fatal" ] && fatal="null"
  heap_start="$(grep -m1 'VPU_HEAP' "$raw" 2>/dev/null | tr -d '\r' || true)"
  heap_final="$(grep 'VPU_HEAP' "$raw" 2>/dev/null | tail -1 | tr -d '\r' || true)"
  frag=$(printf '{"name":"%s","rc":%d,"timeout_s":%d,"duration_s":%d,"ok":%s,"fail":%s,"heap2_delta_mb":%s,"inflight_end":%s,"rtmp_fatal":%s,"heap_start":"%s","heap_final":"%s"},' \
    "$name" "$rc" "$tmo" "$dur" "$ok" "$fail" "$delta" "$inflight" "$fatal" \
    "$(json_str "$heap_start")" "$(json_str "$heap_final")")
  PHASES_JSON="${PHASES_JSON}${frag}"
}

# ---- 运行单阶段：timeout + 捕获 rc + 记录 ----
# run_phase <name> <timeout_s> <tool_args...>
run_phase() {
  local name="$1"; shift
  local tmo="$1"; shift
  local log_file="$LOG_DIR/forced_reconnect_${name}.log"
  local raw="$LOG_DIR/forced_reconnect_${name}.raw"
  local t0 t1 rc=0 dur
  t0="$(date -u +%s)"
  log "=== 阶段 $name 开始 | 超时 ${tmo}s | $TOOL $* ==="
  timeout -k 5 "$tmo" "$TOOL" "$@" >"$raw" 2>&1 || rc=$?
  rc="${rc:-0}"
  t1="$(date -u +%s)"
  dur=$((t1 - t0))
  grep -vE 'dynsym|bm decoder id|bm output format|mode bitstream|BMvidDec|libbmvideo|vpu firmware|VERSION=' "$raw" >"$log_file" 2>/dev/null || cp "$raw" "$log_file"
  if [ "$rc" -eq 124 ]; then
    log "=== 阶段 $name 超时（${tmo}s），视为失败 ==="
  elif [ "$rc" -ne 0 ]; then
    log "=== 阶段 $name 失败 rc=$rc（用时 ${dur}s）==="
  else
    log "=== 阶段 $name 完成 rc=0（用时 ${dur}s）==="
  fi
  record_phase "$name" "$rc" "$tmo" "$dur" "$raw"
  if [ "$rc" -ne 0 ]; then
    OVERALL_RC="$rc"
  fi
  return "$rc"
}

main() {
  log "forced_reconnect 维护窗口测试 | 模式=$MODE 轮次=$ROUNDS"
  if ! refuse_if_production_running; then OVERALL_RC=2; exit 2; fi
  if ! validate_candidate; then OVERALL_RC=2; exit 2; fi

  case "$MODE" in
    heap)
      run_phase heap "$(phase_timeout HEAP 30)" heap || finalize
      ;;
    repro)
      run_phase repro "$(phase_timeout REPRO 180)" repro "${3:-3}" || finalize
      ;;
    decoder)
      run_phase decoder "$(phase_timeout DECODER 1200)" decoder "$ROUNDS" "${3:-20}" || finalize
      ;;
    rtsp)
      log "RTSP-only 复用 decoder 解码器换建路径（文件源模拟 RTSP 解码器换建）"
      run_phase rtsp "$(phase_timeout RTSP 1200)" decoder "$ROUNDS" "${3:-20}" || finalize
      ;;
    rtmp)
      run_phase rtmp "$(phase_timeout RTMP 60)" rtmp || finalize
      ;;
    dual)
      run_phase dual "$(phase_timeout DUAL 1200)" dual "$ROUNDS" || finalize
      ;;
    sweep)
      for buf in 5 8 12 20; do
        run_phase "sweep_${buf}" "$(phase_timeout SWEEP 300)" decoder 20 "$buf" || { log "sweep buf=$buf 失败，停止后续"; finalize; }
      done
      ;;
    all)
      run_phase heap    "$(phase_timeout HEAP 30)"    heap     || { log "heap 失败，停止";    finalize; }
      run_phase repro   "$(phase_timeout REPRO 180)"   repro 3  || { log "repro 失败，停止";   finalize; }
      run_phase decoder "$(phase_timeout DECODER 1200)" decoder 100 20 || { log "decoder 失败，停止"; finalize; }
      run_phase rtsp    "$(phase_timeout RTSP 1200)"   decoder 100 20  || { log "rtsp 失败，停止";    finalize; }
      for buf in 5 8 12 20; do
        run_phase "sweep_${buf}" "$(phase_timeout SWEEP 300)" decoder 20 "$buf" || { log "sweep buf=$buf 失败，停止"; finalize; }
      done
      run_phase dual    "$(phase_timeout DUAL 1200)"   dual 100 || { log "dual 失败，停止";   finalize; }
      run_phase rtmp    "$(phase_timeout RTMP 60)"     rtmp     || { log "rtmp 失败，停止";   finalize; }
      ;;
    *)
      log "用法: $0 all|heap|repro|decoder|rtsp|rtmp|dual|sweep [rounds]"
      OVERALL_RC=2; exit 2
      ;;
  esac
  finalize
}

main "$@"
