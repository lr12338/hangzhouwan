#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 阶段4 稳定性测试脚本。
#
# 用法：
#   ./stability_test.sh [10|60|300|30|120] [--preprocess cpu|bmcv] [--draw-mode cpu|bmcv|none]
#
# 默认推荐配置：--preprocess cpu --draw-mode bmcv
#   （CPU 预处理保证检测正确性，BMCV 绘制消除 sws 往返瓶颈，约 10fps）
# 纯性能测试可用：--preprocess bmcv --draw-mode bmcv
#   （BMCV CSC 与 sws 系数存在差异，检测框 IoU/score 略有偏移，详见 docs/19）
#
# 时长语义（显式区分秒与分钟，避免歧义）：
#   10  ->  10 秒   （Agent 功能测试）
#   60  ->  60 秒   （Agent 性能测试）
#   300 -> 300 秒   （Agent 短时稳定性测试，最长 5 分钟）
#   30  -> 1800 秒  （30 分钟，人工门禁，Agent 不得自动执行）
#   120 -> 7200 秒  （2 小时，人工门禁，Agent 不得自动执行）
#
# 本脚本：
#   - 捕获主程序真实退出码（禁止 wait ... || true 吞码）；
#   - trap 保证退出时停止主程序与监控子进程、删除 PID 文件；
#   - 每 30 秒周期采样 CPU/内存/线程/输出/TPU 到 CSV；
#   - 用 ffprobe 验证输出，失败返回非 0；
#   - 结束后检查无 single_video_infer 残留进程。
# =============================================================================
set -euo pipefail

LEVEL=${1:-300}
PREPROCESS="cpu"     # 推荐 cpu（检测正确性）；bmcv 仅用于性能对比
DRAW_MODE="bmcv"     # 推荐 bmcv（消除 sws 往返瓶颈）
# 解析可选 --preprocess / --draw-mode
while [[ $# -gt 1 ]]; do
  case "$2" in
    --preprocess) PREPROCESS="$3"; shift 2;;
    --draw-mode)  DRAW_MODE="$3";  shift 2;;
    *) break;;
  esac
done
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

BMODEL="artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel"
INPUT="testdata/test.mp4"
OUT_DIR="artifacts/stage4"
LOG_DIR="$OUT_DIR/logs"
mkdir -p "$LOG_DIR"

case "$LEVEL" in
  10)   DURATION=10;   NAME="func_10s";    UNIT="秒" ;;
  60)   DURATION=60;   NAME="perf_60s";    UNIT="秒" ;;
  300)  DURATION=300;  NAME="smoke_5min";  UNIT="秒" ;;
  30)   DURATION=1800; NAME="medium_30min"; UNIT="分钟" ;;
  120)  DURATION=7200; NAME="full_2hour";  UNIT="分钟" ;;
  5)    DURATION=300;  NAME="smoke_5min";  UNIT="秒" ;;   # 向后兼容旧入参 5
  -h|--help)
    sed -n '3,20p' "$0"; exit 0 ;;
  *) echo "用法: $0 [10|60|300|30|120]（10/60/300 为秒，30/120 为分钟）"; exit 1 ;;
esac

OUT_FILE="$OUT_DIR/${NAME}.mp4"
LOG_FILE="$LOG_DIR/${NAME}.log"
CSV_FILE="$LOG_DIR/${NAME}_periodic.csv"
METRICS_BEFORE="$LOG_DIR/${NAME}_before.txt"
METRICS_AFTER="$LOG_DIR/${NAME}_after.txt"
PID_FILE="$LOG_DIR/${NAME}.pid"

export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib

MAIN_PID=""
MON_PID=""
PROCESS_EXIT_CODE=0
CLEANING_UP=0

cleanup() {
  # 重入保护：避免 trap 与正常退出重复清理。
  if [[ "$CLEANING_UP" -eq 1 ]]; then return; fi
  CLEANING_UP=1
  # 停止主程序
  if [[ -n "$MAIN_PID" ]] && kill -0 "$MAIN_PID" 2>/dev/null; then
    kill -INT "$MAIN_PID" 2>/dev/null || true
    sleep 1
    kill -KILL "$MAIN_PID" 2>/dev/null || true
  fi
  # 停止监控子进程
  if [[ -n "$MON_PID" ]] && kill -0 "$MON_PID" 2>/dev/null; then
    kill -KILL "$MON_PID" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
  # 残留检查
  if pgrep -x single_video_infer >/dev/null 2>&1; then
    echo "警告 | 清理 | 仍存在 single_video_infer 残留进程：" | tee -a "$LOG_FILE"
    pgrep -af single_video_infer | tee -a "$LOG_FILE" || true
  fi
}
trap cleanup EXIT INT TERM

snapshot() {
  local tag="$1"
  local file="$2"
  {
    echo "=== $NAME $tag ==="
    echo "--- free -h ---"; free -h
    echo "--- bm-smi -noloop ---"; bm-smi -noloop 2>/dev/null || echo "bm-smi 不可用"
    echo "--- top processes ---"; ps -eo pid,ppid,%cpu,%mem,rss,vsz,cmd --sort=-rss | head -n 10
  } > "$file" 2>&1
}

# 周期资源采样：每 30 秒一行 CSV
periodic_monitor() {
  local start_ts
  start_ts=$(date +%s)
  echo "timestamp,elapsed_sec,pid_alive,cpu_percent,rss_kb,threads,output_bytes,mem_available_kb,tpu_used_mb" > "$CSV_FILE"
  while true; do
    sleep 30
    local now elapsed alive cpu rss thrs obytes mavail tpu
    now=$(date +%s)
    elapsed=$((now - start_ts))
    if [[ -n "$MAIN_PID" ]] && kill -0 "$MAIN_PID" 2>/dev/null; then
      alive=1
      cpu=$(ps -o %cpu= -p "$MAIN_PID" 2>/dev/null | tr -d ' ' || echo "0")
      rss=$(ps -o rss= -p "$MAIN_PID" 2>/dev/null | tr -d ' ' || echo "0")
      thrs=$(ps -o nlwp= -p "$MAIN_PID" 2>/dev/null | tr -d ' ' || echo "0")
    else
      alive=0; cpu=0; rss=0; thrs=0
    fi
    obytes=0
    [[ -f "$OUT_FILE" ]] && obytes=$(stat -c %s "$OUT_FILE" 2>/dev/null || echo 0)
    mavail=$(awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null || echo 0)
    tpu=$(bm-smi -noloop 2>/dev/null | awk '/Tpu/{print $0; exit}' | grep -oE '[0-9]+' | head -1 || echo "NA")
    echo "$(date '+%Y-%m-%dT%H:%M:%S'),$elapsed,$alive,$cpu,$rss,$thrs,$obytes,$mavail,$tpu" >> "$CSV_FILE"
  done
}

echo "===== 阶段4 稳定性测试: $NAME (${DURATION}${UNIT}) =====" | tee "$LOG_FILE"
echo "开始时间: $(date)" | tee -a "$LOG_FILE"

snapshot "测试前" "$METRICS_BEFORE"

# 启动主程序（后台，--max-seconds 控制时长，--loop 0 循环播放）
# 推荐配置：CPU 预处理（检测正确性）+ BMCV 绘制（消除 sws 往返瓶颈）。
./build/single_video_infer \
  --input "$INPUT" --output "$OUT_FILE" --bmodel "$BMODEL" \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --source-fps 20 --output-fps 10 --inference-fps 5 \
  --bitrate-kbps 800 --gop 20 --queue-size 1 \
  --conf 0.1 --iou 0.1 --result-ttl-ms 1000 \
  --loop 0 --max-seconds "$DURATION" --metrics-interval 30 \
  --preprocess "${PREPROCESS}" --draw-mode "${DRAW_MODE}" \
  >> "$LOG_FILE" 2>&1 &
MAIN_PID=$!
echo "$MAIN_PID" > "$PID_FILE"
echo "配置: preprocess=${PREPROCESS} draw_mode=${DRAW_MODE}" | tee -a "$LOG_FILE"
echo "主程序 PID: $MAIN_PID" | tee -a "$LOG_FILE"

# 启动周期采样监控
periodic_monitor &
MON_PID=$!

# 等待主程序结束，捕获真实退出码（禁止吞码）
set +e
wait "$MAIN_PID"
PROCESS_EXIT_CODE=$?
set -e

# 停止监控
if kill -0 "$MON_PID" 2>/dev/null; then
  kill -KILL "$MON_PID" 2>/dev/null || true
  wait "$MON_PID" 2>/dev/null || true
fi
MON_PID=""

echo "主程序退出码: $PROCESS_EXIT_CODE" | tee -a "$LOG_FILE"
echo "结束时间: $(date)" | tee -a "$LOG_FILE"

snapshot "测试后" "$METRICS_AFTER"

# 输出验证（ffprobe 失败视为测试失败）
echo "===== 输出验证 =====" | tee -a "$LOG_FILE"
FFPROBE_OK=0
if ffprobe -v error -show_entries stream=codec_name,width,height,nb_frames,r_frame_rate \
  -show_entries format=duration,bit_rate \
  -of default=noprint_wrappers=1 "$OUT_FILE" 2>"$LOG_DIR/${NAME}_ffprobe.err" \
  | grep -vE '^BMvid|^libbm|^vpu|^VERSION' | tee -a "$LOG_FILE"; then
  FFPROBE_OK=1
else
  echo "ffprobe 失败（见 ${NAME}_ffprobe.err）" | tee -a "$LOG_FILE"
fi

# 残留进程检查
echo "===== 残留进程检查 =====" | tee -a "$LOG_FILE"
if pgrep -x single_video_infer >/dev/null 2>&1; then
  echo "失败 | 存在 single_video_infer 残留进程" | tee -a "$LOG_FILE"
  pgrep -af single_video_infer | tee -a "$LOG_FILE" || true
  PROCESS_EXIT_CODE=${PROCESS_EXIT_CODE:-1}
  [[ "$PROCESS_EXIT_CODE" -eq 0 ]] && PROCESS_EXIT_CODE=1
else
  echo "通过 | 无 single_video_infer 残留进程" | tee -a "$LOG_FILE"
fi

# 关键指标汇总
echo "===== 关键指标汇总 =====" | tee -a "$LOG_FILE"
grep -E '视频管线|结束|主程序退出码|限时' "$LOG_FILE" | tail -12 | tee -a "$LOG_FILE" || true

echo "===== $NAME 完成（退出码=$PROCESS_EXIT_CODE）=====" | tee -a "$LOG_FILE"

# 退出码规则：0 才算通过；ffprobe 失败或残留进程强制非 0。
if [[ "$FFPROBE_OK" -ne 1 ]]; then
  PROCESS_EXIT_CODE=1
fi
exit "$PROCESS_EXIT_CODE"
