#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# 从 testdata/test.mp4 抽取 INT8 校准帧（真实数据）。
# 用法：bash tools/convert_model/extract_calibration.sh [视频路径] [输出目录] [每N秒1帧]
# 依赖：ffmpeg（工控机用 Sophon-FFmpeg，x86 用标准 ffmpeg，CLI 一致）。
# 输出：960x544 JPEG，约 63 帧（125s / 2s），用于 TPU-MLIR run_calibration。
set -euo pipefail

VIDEO="${1:-testdata/test.mp4}"
OUTDIR="${2:-testdata/calibration}"
FPS="${3:-0.5}"   # 每 2 秒 1 帧

FFMPEG="${FFMPEG:-ffmpeg}"
command -v "$FFMPEG" >/dev/null 2>&1 || {
  # 工控机回退到 Sophon-FFmpeg
  FFMPEG=/opt/sophon/sophon-ffmpeg-latest/bin/ffmpeg
}

mkdir -p "$OUTDIR"
echo "信息 | 校准帧抽取 | 源=$VIDEO 输出=$OUTDIR fps=$FPS"
"$FFMPEG" -hide_banner -loglevel error -y -i "$VIDEO" \
  -vf "fps=${FPS}" -q:v 2 "$OUTDIR/frame_%04d.jpg"

COUNT=$(ls "$OUTDIR"/frame_*.jpg 2>/dev/null | wc -l)
echo "信息 | 校准帧抽取 | 完成，共 $COUNT 帧"
