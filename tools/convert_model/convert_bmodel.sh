#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# best.onnx -> BM1684 bmodel 转换脚本（须在 x86 TPU-MLIR 环境执行）
# -----------------------------------------------------------------------------
# 约束（来自阶段1/2 目标）：
#   - 目标处理器 BM1684（不得用 BM1684X）
#   - 模型路线 F32 基线 -> 板端验证 -> INT8 校准与验证（不得 FP16）
#   - TPU-MLIR 版本须与板端 libsophon 0.4.9 匹配
#   - 工控机(SoC)无 TPU-MLIR，转换只能在 x86 进行
#
# 审计结论（tools/inspect_onnx.py，详见 docs/06）：
#   - 输入名 images，动态形状 [batch,3,height,width] -> 固化为 [1,3,640,640]
#   - 输出名 output（模型不含 NMS，后处理须在 C++ 侧实现）
#   - opset 12 / ir 7，算子均为 TPU-MLIR 常见支持算子
#   - 预处理（resize/RGB//255/转置）在 C++ 侧完成，bmodel 仅做纯推理
# =============================================================================
set -euo pipefail

# ---------- 可配置参数 ----------
MODEL_NAME="${MODEL_NAME:-yolov7_ship}"
ONNX="${ONNX:-weights/best.onnx}"
CALIB_DIR="${CALIB_DIR:-testdata/calibration}"
CALIB_NUM="${CALIB_NUM:-63}"
WORK_DIR="${WORK_DIR:-build/convert}"     # 转换工作区（产物，已 gitignore）
OUT_DIR="${OUT_DIR:-weights}"             # bmodel 输出目录
INPUT_NAMES="${INPUT_NAMES:-images}"
OUTPUT_NAMES="${OUTPUT_NAMES:-output}"
INPUT_SHAPES="${INPUT_SHAPES:-[[1,3,640,640]]}"
PROCESSOR="${PROCESSOR:-BM1684}"           # 必须 BM1684

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

echo "============================================================"
echo "阶段1：model_transform（ONNX -> MLIR，固化输入形状）"
echo "============================================================"
model_transform \
  --model_name "$MODEL_NAME" \
  --model_def "../$ONNX" \
  --input_shapes "$INPUT_SHAPES" \
  --input_names "$INPUT_NAMES" \
  --output_names "$OUTPUT_NAMES" \
  --mlir "${MODEL_NAME}.mlir"

echo "============================================================"
echo "阶段2：model_deploy F32（MLIR -> BM1684 F32 基线 bmodel）"
echo "============================================================"
model_deploy \
  --mlir "${MODEL_NAME}.mlir" \
  --processor "$PROCESSOR" \
  --mode F32 \
  --model "${OUT_DIR}/${MODEL_NAME}_1684_f32.bmodel"

echo "============================================================"
echo "阶段3：run_calibration（真实数据生成 INT8 校准表）"
echo "  预处理：resize 640x640 / RGB / mean=0 / scale=1/255"
echo "============================================================"
run_calibration "${MODEL_NAME}.mlir" \
  --dataset "../$CALIB_DIR" \
  --input_num "$CALIB_NUM" \
  --preprocess \
  --resize_channel 3 --resize_w 640 --resize_h 640 \
  --c "rgb" \
  --mean "0,0,0" \
  --scale "0.003921569,0.003921569,0.003921569" \
  -o "${MODEL_NAME}_calib.table"

echo "============================================================"
echo "阶段4：model_deploy INT8（MLIR -> BM1684 INT8 bmodel，带校准表）"
echo "============================================================"
model_deploy \
  --mlir "${MODEL_NAME}.mlir" \
  --processor "$PROCESSOR" \
  --mode INT8 \
  --calibration_table "${MODEL_NAME}_calib.table" \
  --model "${OUT_DIR}/${MODEL_NAME}_1684_int8.bmodel"

echo "============================================================"
echo "转换完成。产物："
echo "  F32 基线 : ${OUT_DIR}/${MODEL_NAME}_1684_f32.bmodel"
echo "  INT8 生产: ${OUT_DIR}/${MODEL_NAME}_1684_int8.bmodel"
echo "------------------------------------------------------------"
echo "下一步（板端 BM1684）："
echo "  1) 拷贝 *_1684_f32.bmodel 到工控机 weights/"
echo "  2) python3 不适用；用 bmrt_test 或 tools/image_inference/bmrt_load_test 验证加载"
echo "  3) F32 精度通过后，再验证 INT8 精度（与 F32/原 ONNX 对比）"
echo "============================================================"
