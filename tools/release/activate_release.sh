#!/bin/bash
# -*- coding: utf-8 -*-
# 原子激活 Release（软链接切换 current -> 新 Release）。
#
# 用法：
#   bash tools/release/activate_release.sh <release_dir>
#
# 使用 ln -sfn 原子切换，不停止正在运行的服务（需手动 restart）。
# 自动备份 previous 指针。

set -euo pipefail

RELEASE_DIR="${1:-}"
if [ -z "$RELEASE_DIR" ] || [ ! -d "$RELEASE_DIR" ]; then
  echo "用法: bash tools/release/activate_release.sh <release_dir>"
  exit 1
fi
RELEASE_DIR="$(cd "$RELEASE_DIR" && pwd)"
BASE_DIR="/opt/hangzhouwan"
CURRENT_LINK="$BASE_DIR/current"
PREVIOUS_LINK="$BASE_DIR/previous"

echo "=== 激活 Release: ${RELEASE_DIR} ==="

mkdir -p "$BASE_DIR"

# 预检（如果 hzwctl 可用）
if [ -x "$RELEASE_DIR/bin/hzwctl" ]; then
  if ! "$RELEASE_DIR/bin/hzwctl" preflight 2>/dev/null; then
    echo "❌ 预检未通过，拒绝激活"
    exit 1
  fi
fi

# 备份 previous
if [ -L "$CURRENT_LINK" ]; then
  OLD_TARGET="$(readlink -f "$CURRENT_LINK")"
  ln -sfn "$OLD_TARGET" "$PREVIOUS_LINK"
  echo "  previous -> $OLD_TARGET"
fi

# 原子切换 current
ln -sfn "$RELEASE_DIR" "$CURRENT_LINK"
echo "  current -> $RELEASE_DIR"

echo ""
echo "=== 激活完成 ==="
echo "当前 Release: $(cat "$RELEASE_DIR/VERSION" | head -1)"
if [ -L "$PREVIOUS_LINK" ]; then
  echo "回滚: bash tools/release/rollback_release.sh"
fi
echo "重启服务: sudo systemctl restart hangzhouwan.target"
