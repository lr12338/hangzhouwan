#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 板端 forced-reconnect 测试运行器（维护窗口执行，生产已停止时运行）。
#
# 记录每轮 VPU Heap、在途帧、重连结果，分别覆盖 decoder-only / RTSP-only /
# RTMP-only / 完整双路 / extra_buffer 扫描。工具内置 VPU 显存熔断（任一堆
# avail < 60MB 中止），防止压垮设备。
#
# 用法：
#   bash tools/dual_stream/forced_reconnect_run.sh [mode] [rounds]
#     mode: all(默认) | decoder | rtsp | rtmp | dual | sweep | repro | heap
#     rounds: 重连次数（默认 decoder/dual=100，sweep 每档 20，repro=3）
#
# 前置：已构建 build/forced_reconnect_tool；生产已停止（释放 VPU 预算）。
# 输出：artifacts/internal-development/forced_reconnect_<mode>.log
# =============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"
LOG_DIR="artifacts/internal-development"
mkdir -p "$LOG_DIR"

MODE="${1:-all}"
ROUNDS="${2:-100}"
export LD_LIBRARY_PATH="/opt/sophon/libsophon-0.4.9/lib:/opt/sophon/sophon-ffmpeg_0.8.0/lib:${LD_LIBRARY_PATH:-}"
TOOL="./build/forced_reconnect_tool"

run() {
  local sub="$1"; shift
  local log="$LOG_DIR/forced_reconnect_${sub}.log"
  echo "=== ${sub} | $(date -u +%FT%TZ) ===" | tee "$log"
  "$TOOL" "$@" 2>&1 | grep -vE "dynsym|bm decoder id|bm output format|mode bitstream|BMvidDec|libbmvideo|vpu firmware|VERSION=" | tee -a "$log"
  echo "=== ${sub} done ===" | tee -a "$log"
}

case "$MODE" in
  heap)   run heap heap ;;
  repro)  run repro repro "${3:-3}" ;;
  decoder) run decoder decoder "$ROUNDS" "${3:-20}" ;;
  rtsp)   echo "RTSP-only 复用 decoder 解码器换建路径（文件源模拟，见 repro/decoder）"; run decoder decoder "$ROUNDS" "${3:-20}" ;;
  rtmp)   run rtmp rtmp ;;
  dual)   run dual dual "$ROUNDS" ;;
  sweep)  run sweep sweep ;;
  all)
    run heap heap
    run repro repro 3
    run decoder decoder 100 20
    run sweep sweep
    run dual dual 100
    run rtmp rtmp
    ;;
  *) echo "用法: $0 all|heap|repro|decoder|rtsp|rtmp|dual|sweep [rounds]"; exit 1 ;;
esac
echo "完成 | 日志: $LOG_DIR/forced_reconnect_*.log"
