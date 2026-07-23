#!/bin/bash
# -*- coding: utf-8 -*-
# =============================================================================
# 手动回滚 Release（current <-> previous 软链接切换 + 重启 + readiness 验证）。
#
# 用法：
#   bash tools/release/rollback_release.sh
#
# 不使用 git checkout，不重新编译。仅切换软链接 + 重启 + 验证。
# =============================================================================

set -euo pipefail

BASE_DIR="/opt/hangzhouwan"
CURRENT_LINK="$BASE_DIR/current"
PREVIOUS_LINK="$BASE_DIR/previous"
TARGET_SVC="hangzhouwan.target"

echo "=== 回滚 Release ==="

if [ ! -L "$PREVIOUS_LINK" ]; then
  echo "❌ 没有 previous Release 可回滚"
  echo "  current -> $(readlink -f "$CURRENT_LINK" 2>/dev/null || echo '无')"
  exit 1
fi

PREV_TARGET="$(readlink -f "$PREVIOUS_LINK")"
CURR_TARGET="$(readlink -f "$CURRENT_LINK")"

echo "  当前:   $CURR_TARGET"
echo "  回滚到: $PREV_TARGET"

# 交换：previous 指向当前 current，current 指向 previous
ln -sfn "$CURR_TARGET" "$PREVIOUS_LINK"
ln -sfn "$PREV_TARGET" "$CURRENT_LINK"

echo ""
echo "  current -> $(readlink -f "$CURRENT_LINK")"
echo "  previous -> $(readlink -f "$PREVIOUS_LINK")"

# 重启服务（先 reset-failed 清除 StartLimitBurst，避免 restart 被抑制）
echo "  重启 hangzhouwan.target..."
sudo systemctl reset-failed "$TARGET_SVC" 2>/dev/null || true
sudo systemctl reset-failed hangzhouwan-business.service 2>/dev/null || true
sudo systemctl reset-failed hangzhouwan-video.service 2>/dev/null || true
sudo systemctl restart "$TARGET_SVC" 2>/dev/null || echo "  ⚠️  systemctl restart 失败（可能 systemd 不可用）"

# 验证 readiness
HZWCTL="${CURRENT_LINK}/bin/hzwctl"
if [ -x "$HZWCTL" ]; then
  sleep 5
  echo "  验证 Business readiness..."
  if "$HZWCTL" wait-business --timeout 30 2>/dev/null; then
    echo "  ✅ Business 就绪"
  else
    echo "  ⚠️  Business 未就绪"
  fi
  echo "  验证 Video readiness..."
  if "$HZWCTL" wait-video --timeout 60 2>/dev/null; then
    echo "  ✅ Video 就绪"
  else
    echo "  ⚠️  Video 未就绪"
  fi
fi

echo ""
echo "=== 回滚完成 ==="
