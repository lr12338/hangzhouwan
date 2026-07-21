#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 阶段4 稳定性测试脚本：5分钟 / 30分钟 / 2小时。
# 用法：./stability_test.sh [5|30|120]
# 记录：运行日志、CPU/内存/TPU 指标、输出视频。
# =============================================================================
set -e

LEVEL=${1:-5}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT_DIR"

BMODEL="artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel"
INPUT="testdata/test.mp4"
OUT_DIR="artifacts/stage4"
LOG_DIR="$OUT_DIR/logs"
mkdir -p "$LOG_DIR"

case "$LEVEL" in
  5)   DURATION=300;  NAME="smoke_5min"  ;;
  30)  DURATION=1800; NAME="medium_30min" ;;
  120) DURATION=7200; NAME="full_2hour"  ;;
  *) echo "用法: $0 [5|30|120]"; exit 1 ;;
esac

OUT_FILE="$OUT_DIR/${NAME}.mp4"
LOG_FILE="$LOG_DIR/${NAME}.log"
METRICS_BEFORE="$LOG_DIR/${NAME}_before.txt"
METRICS_AFTER="$LOG_DIR/${NAME}_after.txt"
PID_FILE="$LOG_DIR/${NAME}.pid"

export LD_LIBRARY_PATH=/opt/sophon/sophon-ffmpeg_0.8.0/lib:/opt/sophon/libsophon-0.4.9/lib

echo "===== 阶段4 稳定性测试: $NAME (${DURATION}s) =====" | tee "$LOG_FILE"
echo "开始时间: $(date)" | tee -a "$LOG_FILE"

# 测试前资源快照
{
  echo "=== $NAME 测试前 ==="
  echo "--- free -h ---"; free -h
  echo "--- bm-smi -noloop ---"; bm-smi -noloop 2>/dev/null || echo "bm-smi 不可用"
  echo "--- top processes ---"; ps -eo pid,ppid,%cpu,%mem,rss,vsz,cmd --sort=-rss | head -n 10
} > "$METRICS_BEFORE" 2>&1

# 启动视频推理（后台，--max-seconds 控制时长，--loop 0 循环播放）
./build/single_video_infer \
  --input "$INPUT" --output "$OUT_FILE" --bmodel "$BMODEL" \
  --device 0 --decoder h264_bm --encoder h264_bm \
  --output-fps 10 --inference-fps 5 --bitrate-kbps 800 --gop 20 \
  --queue-size 1 --conf 0.1 --iou 0.1 --result-ttl-ms 1000 \
  --loop 0 --max-seconds "$DURATION" --metrics-interval 30 \
  >> "$LOG_FILE" 2>&1 &
PID=$!
echo $PID > "$PID_FILE"
echo "进程 PID: $PID" | tee -a "$LOG_FILE"

# 等待完成
wait $PID 2>/dev/null || true
echo "结束时间: $(date)" | tee -a "$LOG_FILE"

# 测试后资源快照
{
  echo "=== $NAME 测试后 ==="
  echo "--- free -h ---"; free -h
  echo "--- bm-smi -noloop ---"; bm-smi -noloop 2>/dev/null || echo "bm-smi 不可用"
  echo "--- top processes ---"; ps -eo pid,ppid,%cpu,%mem,rss,vsz,cmd --sort=-rss | head -n 10
} > "$METRICS_AFTER" 2>&1

# 验证输出
echo "===== 输出验证 =====" | tee -a "$LOG_FILE"
ffprobe -v error -show_entries stream=codec_name,width,height,nb_frames,r_frame_rate \
  -show_entries format=duration,bit_rate \
  -of default=noprint_wrappers=1 "$OUT_FILE" 2>/dev/null \
  | grep -vE '^BMvid|^libbm|^vpu|^VERSION' | tee -a "$LOG_FILE" || echo "ffprobe 失败" | tee -a "$LOG_FILE"

# 汇总关键指标
echo "===== 关键指标汇总 =====" | tee -a "$LOG_FILE"
grep -E '视频管线|结束' "$LOG_FILE" | tail -10 | tee -a "$LOG_FILE"

rm -f "$PID_FILE"
echo "===== $NAME 完成 =====" | tee -a "$LOG_FILE"
