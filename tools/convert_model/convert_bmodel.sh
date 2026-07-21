#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# 在 x86 TPU-MLIR Docker 容器内执行。默认只生成 BM1684 F32 bmodel。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${MODE:-f32}"
MODEL_NAME="${MODEL_NAME:-yolov7_ship}"
ONNX="${ONNX:-${PROJECT_ROOT}/hangzhouwan_beishang/weights/best.onnx}"
CALIB_DIR="${CALIB_DIR:-${PROJECT_ROOT}/testdata/calibration}"
INPUT_SHAPES='[[1,3,640,640]]'
INPUT_NAME='images'
OUTPUT_NAME='output'
PROCESSOR='BM1684'

case "$MODE" in
  f32)
    ARTIFACT_DIR="${ARTIFACT_DIR:-${PROJECT_ROOT}/artifacts/bm1684-f32}"
    ;;
  int8)
    ARTIFACT_DIR="${ARTIFACT_DIR:-${PROJECT_ROOT}/artifacts/bm1684-int8}"
    ;;
  *)
    printf '%s | 错误 | 转换 | MODE 仅支持 f32 或 int8，当前值：%s\n' "$(date '+%F %T')" "$MODE" >&2
    exit 2
    ;;
esac

WORK_DIR="${WORK_DIR:-${ARTIFACT_DIR}/work}"
LOG_FILE="${ARTIFACT_DIR}/conversion.log"
MODEL_FILE="${ARTIFACT_DIR}/${MODEL_NAME}_1684_${MODE}.bmodel"

mkdir -p "$ARTIFACT_DIR" "$WORK_DIR"
exec > >(tee "$LOG_FILE") 2>&1

log() {
  printf '%s | %s | %s | %s\n' "$(date '+%F %T')" "$1" "$2" "$3"
}

run_logged() {
  local module="$1"
  shift
  log 信息 "$module" "执行：$*"
  "$@" 2>&1 | while IFS= read -r line; do
    log 信息 "$module" "$line"
  done
}

require_tool() {
  local tool="$1"
  if ! command -v "$tool" >/dev/null 2>&1; then
    log 错误 环境 "缺少 TPU-MLIR 工具：$tool"
    exit 1
  fi
}

find_validation_image() {
  if [[ -n "${TEST_IMAGE:-}" ]]; then
    printf '%s\n' "$TEST_IMAGE"
    return
  fi
  find "$CALIB_DIR" -maxdepth 1 -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \) -print -quit 2>/dev/null || true
}

for tool in python3 model_transform.py model_deploy.py model_tool; do
  require_tool "$tool"
done

if [[ ! -s "$ONNX" ]]; then
  log 错误 输入 "ONNX 文件不存在或为空：$ONNX"
  exit 1
fi

TEST_IMAGE="$(find_validation_image)"
if [[ -z "$TEST_IMAGE" || ! -s "$TEST_IMAGE" ]]; then
  log 错误 输入 "未找到验证图片。请同步 testdata/calibration，或设置 TEST_IMAGE 为一张本地图片。"
  exit 1
fi

log 信息 环境 "模式=${MODE}；目标=${PROCESSOR}；输入=${INPUT_NAME}${INPUT_SHAPES}；输出=${OUTPUT_NAME}"
log 信息 输入 "ONNX=${ONNX}；验证图片=${TEST_IMAGE}"
run_logged 工具链 model_transform.py --help
run_logged 工具链 model_deploy.py --help

ONNX_AUDIT="${WORK_DIR}/onnx-audit.txt"
python3 "${PROJECT_ROOT}/tools/inspect_onnx.py" "$ONNX" > "$ONNX_AUDIT" 2>&1
while IFS= read -r line; do
  log 信息 审计 "$line"
done < "$ONNX_AUDIT"
if ! grep -Fq "${INPUT_NAME} : FLOAT [batch,3,height,width]" "$ONNX_AUDIT" || ! grep -Fq "${OUTPUT_NAME} : FLOAT" "$ONNX_AUDIT"; then
  log 错误 审计 "ONNX 输入或输出名称与受控转换参数不符，停止。"
  exit 1
fi

MLIR_FILE="${WORK_DIR}/${MODEL_NAME}.mlir"
INPUT_NPZ="${WORK_DIR}/${MODEL_NAME}_in_f32.npz"
REFERENCE_NPZ="${WORK_DIR}/${MODEL_NAME}_top_outputs.npz"

# 预处理与旧 detector.py 一致；模型输入仍为 FLOAT，C++ 侧也须同样完成 RGB/resize/1/255。
run_logged 转换 model_transform.py \
  --model_name "$MODEL_NAME" \
  --model_def "$ONNX" \
  --input_shapes "$INPUT_SHAPES" \
  --mean 0.0,0.0,0.0 \
  --scale 0.003921568627,0.003921568627,0.003921568627 \
  --pixel_format rgb \
  --output_names "$OUTPUT_NAME" \
  --test_input "$TEST_IMAGE" \
  --test_result "$REFERENCE_NPZ" \
  --mlir "$MLIR_FILE"

if [[ ! -s "$MLIR_FILE" || ! -s "$INPUT_NPZ" || ! -s "$REFERENCE_NPZ" ]]; then
  log 错误 转换 "MLIR 或 ONNX 参考输出生成失败，停止。"
  exit 1
fi

if [[ "$MODE" == 'f32' ]]; then
  run_logged 转换 model_deploy.py \
    --mlir "$MLIR_FILE" \
    --quantize F32 \
    --processor "$PROCESSOR" \
    --test_input "$INPUT_NPZ" \
    --test_reference "$REFERENCE_NPZ" \
    --model "$MODEL_FILE"
else
  require_tool run_calibration.py
  if [[ ! -d "$CALIB_DIR" ]]; then
    log 错误 输入 "INT8 校准目录不存在：$CALIB_DIR"
    exit 1
  fi
  CALIBRATION_TABLE="${WORK_DIR}/${MODEL_NAME}_calib.table"
  run_logged 转换 run_calibration.py "$MLIR_FILE" \
    --dataset "$CALIB_DIR" \
    --input_num "${CALIB_NUM:-63}" \
    -o "$CALIBRATION_TABLE"
  run_logged 转换 model_deploy.py \
    --mlir "$MLIR_FILE" \
    --quantize INT8 \
    --processor "$PROCESSOR" \
    --calibration_table "$CALIBRATION_TABLE" \
    --test_input "$INPUT_NPZ" \
    --test_reference "$REFERENCE_NPZ" \
    --model "$MODEL_FILE"
fi

if [[ ! -s "$MODEL_FILE" ]]; then
  log 错误 验证 "bmodel 未生成或为空，停止。"
  exit 1
fi

MODEL_INFO="${WORK_DIR}/model-info.txt"
model_tool --info "$MODEL_FILE" > "$MODEL_INFO" 2>&1
while IFS= read -r line; do
  log 信息 验证 "$line"
done < "$MODEL_INFO"
if ! grep -qi 'BM1684' "$MODEL_INFO"; then
  log 错误 验证 "model_tool 未确认目标芯片为 BM1684，停止。"
  exit 1
fi
if ! grep -Fq "$INPUT_NAME" "$MODEL_INFO" || ! grep -Fq "$OUTPUT_NAME" "$MODEL_INFO"; then
  log 错误 验证 "model_tool 未确认输入 images 或输出 output，停止。"
  exit 1
fi
if ! grep -Eqi '1[^0-9]+3[^0-9]+640[^0-9]+640' "$MODEL_INFO"; then
  log 错误 验证 "model_tool 未确认静态输入 Shape [1,3,640,640]，停止。"
  exit 1
fi
sha256sum "$MODEL_FILE" > "${ARTIFACT_DIR}/sha256sum.txt"
{
  printf 'model_transform.py: '; model_transform.py --version 2>&1 || true
  printf 'model_deploy.py: '; model_deploy.py --version 2>&1 || true
  printf 'model_tool: '; model_tool --version 2>&1 || true
  uname -a
} > "${ARTIFACT_DIR}/toolchain-version.txt"
cat > "${ARTIFACT_DIR}/model-manifest.json" <<EOF
{
  "model": "$(basename "$MODEL_FILE")",
  "processor": "BM1684",
  "quantize": "${MODE^^}",
  "input": {"name": "${INPUT_NAME}", "shape": [1, 3, 640, 640], "type": "FLOAT"},
  "output": {"name": "${OUTPUT_NAME}"},
  "source_onnx_sha256": "$(sha256sum "$ONNX" | awk '{print $1}')",
  "validation": "ONNX reference vs MLIR/F32 raw tensor comparison completed by model_deploy.py"
}
EOF
log 信息 验证 "转换和工具链数值验证完成：$MODEL_FILE"
