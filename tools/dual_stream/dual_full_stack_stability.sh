#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 双路全链路稳定性测试脚本（人工执行）。
#
# 用法：
#   ./tools/dual_stream/dual_full_stack_stability.sh 30   # 30 分钟（1800秒）
#   ./tools/dual_stream/dual_full_stack_stability.sh 120  # 2 小时（7200秒）
#
# Agent 不得自动执行超过 300 秒的测试。本脚本仅供人工监控使用。
#
# 前置条件：
#   - 已构建 build/dual_stream_app
#   - 环境变量 STREAM_A/B_INPUT_URL、STREAM_A/B_OUTPUT_URL 已设置
#   - 业务 Sidecar 可用（自动启动）
#
# 监控命令：
#   tail -f artifacts/internal-development/dual_full_stack.log
#   watch -n 5 bm-smi -noloop
#   watch -n 5 free -h
#   watch -n 5 "ps -eo pid,ppid,%cpu,%mem,rss,vsz,nlwp,cmd --sort=-rss | head -20"
#   tail -f artifacts/internal-development/dual_metrics.csv
# =============================================================================
set -euo pipefail

MINUTES=${1:-30}
case "$MINUTES" in
  30)  DURATION=1800; LABEL="30min" ;;
  120) DURATION=7200; LABEL="2hour" ;;
  *)   echo "用法: $0 30|120（30=30分钟，120=2小时）"; exit 1 ;;
esac

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

LOG_DIR="artifacts/internal-development"
mkdir -p "$LOG_DIR"

LOG_FILE="$LOG_DIR/dual_full_stack.log"
METRICS_CSV="$LOG_DIR/dual_metrics.csv"
JSONL_A="$LOG_DIR/stream_A_events.jsonl"
JSONL_B="$LOG_DIR/stream_B_events.jsonl"

# 清理旧文件
: > "$LOG_FILE"
: > "$JSONL_A"
: > "$JSONL_B"
echo "timestamp,A_output,A_infer,A_drops,B_output,B_infer,B_drops,rss_mb,tpu_mb" > "$METRICS_CSV"

SOCK="/tmp/hangzhouwan-business.sock"

echo "信息 | 启动双路全链路稳定性测试 | 时长=${DURATION}秒（${LABEL}）"

# 启动业务 Sidecar
export COORD_MODEL_A="${COORD_MODEL_A:-weights/0121_random_forest_model.pkl}"
export COORD_MODEL_B="${COORD_MODEL_B:-weights/beishang_x-l.pkl}"
export HANGZHOUWAN_BUSINESS_SOCK="$SOCK"
python3 tools/business/business_sidecar.py > "$LOG_DIR/sidecar_full_stack.log" 2>&1 &
SIDECAR_PID=$!

# 等待 Sidecar
for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
if [ ! -S "$SOCK" ]; then
  echo "错误 | Sidecar 未就绪"
  kill $SIDECAR_PID 2>/dev/null || true
  exit 1
fi
sleep 2

# 资源监控函数
monitor() {
  while true; do
    local ts=$(date '+%Y-%m-%dT%H:%M:%S')
    local rss=$(ps -eo rss --sort=-rss | head -2 | tail -1 | tr -d ' ')
    local rss_mb=$((rss / 1024))
    local tpu_mb=$(bm-smi -noloop 2>/dev/null | grep -oP '\d+M' | head -1 | tr -d 'M' || echo 0)
    echo "$ts,,,,,,,$rss_mb,$tpu_mb" >> "$METRICS_CSV"
    sleep 30
  done
}
monitor &
MONITOR_PID=$!

# 清理函数
cleanup() {
  echo "信息 | 清理中..."
  kill $SIDECAR_PID 2>/dev/null || true
  kill $MONITOR_PID 2>/dev/null || true
  wait $SIDECAR_PID 2>/dev/null || true
  wait $MONITOR_PID 2>/dev/null || true
  # 检查残留进程
  pgrep -af 'dual_stream_app|business_sidecar' || echo "信息 | 无残留进程"
  # dmesg 检查
  dmesg 2>/dev/null | grep -Ei 'oom|killed process|bm|vpu|error' | tail -20 || true
}
trap cleanup EXIT

# 生成临时开发配置（生产使用 /etc/hangzhouwan/application.yaml）
DEV_CONFIG="$LOG_DIR/dev_test_config.yaml"
cat > "$DEV_CONFIG" <<CFGEOF
application:
  environment: development
inference:
  model_path: "$ROOT_DIR/artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel"
  device: 0
  confidence_threshold: 0.1
  iou_threshold: 0.1
streams:
  - id: A
    enabled: true
    input_url_env: STREAM_A_INPUT_URL
    output_url_env: STREAM_A_OUTPUT_URL
    coordinate_model: "$ROOT_DIR/weights/0121_random_forest_model.pkl"
    output_fps: 10
    inference_fps: 5
    output_bitrate_kbps: 800
    gop: 20
    jitter_buffer_size: 5
    forbidden_rectangles: [[1480, 0, 2560, 630]]
    forbidden_polygons: [[[0, 0], [0, 640], [710, 620]]]
  - id: B
    enabled: true
    input_url_env: STREAM_B_INPUT_URL
    output_url_env: STREAM_B_OUTPUT_URL
    coordinate_model: "$ROOT_DIR/weights/beishang_x-l.pkl"
    output_fps: 10
    inference_fps: 5
    output_bitrate_kbps: 800
    gop: 20
    forbidden_rectangles: [[0, 0, 2560, 210]]
    forbidden_polygons: [[[2160, 210], [2560, 280], [2560, 210]]]
business:
  coordinate_mode: sklearn
  model_a_path: "$ROOT_DIR/weights/0121_random_forest_model.pkl"
  model_b_path: "$ROOT_DIR/weights/beishang_x-l.pkl"
  socket_path: "$SOCK"
  ais_max_distance_m:
    A: 500
    B: 500
runtime:
  decoder: h264_bm
  encoder: h264_bm
  preprocess: bmcv
  draw_mode: bmcv
CFGEOF

# 启动双路应用（配置驱动）
./build/dual_stream_app \
  --config "$DEV_CONFIG" \
  --max-seconds $DURATION \
  --metrics-interval 10 \
  --enable-business \
  --business-socket "$SOCK" \
  2>&1 | tee -a "$LOG_FILE"

EXIT_CODE=${PIPESTATUS[0]}
echo "信息 | 测试结束 | 退出码=$EXIT_CODE | JSONL_A=$(wc -l < "$JSONL_A") JSONL_B=$(wc -l < "$JSONL_B")"

# 人工验收检查
echo "=== 人工验收检查 ==="
echo "RSS 增长: $(tail -1 "$METRICS_CSV" | cut -d, -f8)MB (30min<100MB, 2h<200MB)"
echo "JSONL A: $(wc -l < "$JSONL_A") 行"
echo "JSONL B: $(wc -l < "$JSONL_B") 行"
echo "残留进程: $(pgrep -af 'dual_stream_app|business_sidecar' || echo '无')"

exit $EXIT_CODE
