#!/bin/bash
# -*- coding: utf-8 -*-
# 验证 Release 制品完整性（SHA256 + manifest）。
#
# 用法：
#   bash tools/release/verify_release.sh <release_dir>
#   bash tools/release/verify_release.sh /opt/hangzhouwan/current

set -euo pipefail

RELEASE_DIR="${1:-/opt/hangzhouwan/current}"
if [ ! -d "$RELEASE_DIR" ]; then
  echo "错误: Release 目录不存在: $RELEASE_DIR"
  exit 1
fi

# 解析符号链接
RELEASE_DIR="$(cd "$RELEASE_DIR" && pwd)"
PASS=0
FAIL=0

echo "=== 验证 Release: ${RELEASE_DIR} ==="

# 1. VERSION 存在
if [ -f "$RELEASE_DIR/VERSION" ]; then
  echo "  ✅ VERSION: $(cat "$RELEASE_DIR/VERSION" | head -1)"
  PASS=$((PASS + 1))
else
  echo "  ❌ VERSION 缺失"
  FAIL=$((FAIL + 1))
fi

# 2. manifest.json 存在且可解析
if [ -f "$RELEASE_DIR/manifest.json" ] && python3 -c "import json; json.load(open('$RELEASE_DIR/manifest.json'))" 2>/dev/null; then
  echo "  ✅ manifest.json 合法"
  PASS=$((PASS + 1))
else
  echo "  ❌ manifest.json 缺失或非法"
  FAIL=$((FAIL + 1))
fi

# 3. sha256sum.txt 存在
if [ ! -f "$RELEASE_DIR/sha256sum.txt" ]; then
  echo "  ❌ sha256sum.txt 缺失"
  FAIL=$((FAIL + 1))
else
  echo "  ✅ sha256sum.txt 存在 ($(wc -l < "$RELEASE_DIR/sha256sum.txt") 条)"
  PASS=$((PASS + 1))
fi

# 4. 校验所有 SHA256
echo "  校验 SHA256..."
cd "$RELEASE_DIR"
if sha256sum -c sha256sum.txt --quiet 2>/dev/null; then
  echo "  ✅ 所有文件 SHA256 校验通过"
  PASS=$((PASS + 1))
else
  echo "  ❌ SHA256 校验失败（包括自包含 venv）"
  FAIL=$((FAIL + 1))
fi

# 5. 关键文件存在
for f in bin/dual_stream_app bin/bridge_capture_app models/yolov7_ship_1684_f32.bmodel config/application.example.yaml config/bridge-capture.env.example systemd/hangzhouwan-bridge-capture.service; do
  if [ -f "$RELEASE_DIR/$f" ]; then
    echo "  ✅ $f"
    PASS=$((PASS + 1))
  else
    echo "  ❌ $f 缺失"
    FAIL=$((FAIL + 1))
  fi
done

# 6. dual_stream_app 可执行
if [ -x "$RELEASE_DIR/bin/dual_stream_app" ]; then
  echo "  ✅ dual_stream_app 可执行"
  PASS=$((PASS + 1))
else
  echo "  ❌ dual_stream_app 不可执行"
  FAIL=$((FAIL + 1))
fi
if [ -x "$RELEASE_DIR/bin/bridge_capture_app" ]; then
  echo "  ✅ bridge_capture_app 可执行"
  PASS=$((PASS + 1))
else
  echo "  ❌ bridge_capture_app 不可执行"
  FAIL=$((FAIL + 1))
fi

# 7. 不可变权限与 Python 运行时独立性
if [ "$(stat -c '%U:%G' "$RELEASE_DIR")" = "root:root" ] &&
   ! find "$RELEASE_DIR" \( ! -user root -o ! -group root \) -print -quit |
     grep -q . &&
   ! find "$RELEASE_DIR" \( -type f -o -type d \) -perm /022 \
     -print -quit | grep -q .; then
  echo "  ✅ Release root:root 且服务只读"
  PASS=$((PASS + 1))
else
  echo "  ❌ Release 权限不符合 root:root 只读"
  FAIL=$((FAIL + 1))
fi
if (cd "$RELEASE_DIR" &&
    PYTHONNOUSERSITE=1 "$RELEASE_DIR/venv/bin/python3" -c \
      "import sys,yaml,numpy,scipy,sklearn,joblib,pyais,paho.mqtt.client; assert not any(p.startswith('/home/') for p in sys.path)") 2>/dev/null; then
  echo "  ✅ Python 自包含且不引用 /home"
  PASS=$((PASS + 1))
else
  echo "  ❌ Python 运行时仍有外部依赖"
  FAIL=$((FAIL + 1))
fi

echo ""
echo "=== 验证结果: $PASS 通过, $FAIL 失败 ==="
exit $FAIL
