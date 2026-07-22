#!/bin/bash
# -*- coding: utf-8 -*-
# 原子回滚 Release（current <-> previous 软链接切换）。
#
# 用法：
#   bash tools/release/rollback_release.sh
#
# 不使用 git checkout，不重新编译。仅切换软链接。
# 需要手动 restart 服务。

set -euo pipefail

BASE_DIR="/opt/hangzhouwan"
CURRENT_LINK="$BASE_DIR/current"
PREVIOUS_LINK="$BASE_DIR/previous"

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
echo "=== 回滚完成 ==="
echo "  current -> $(readlink -f "$CURRENT_LINK")"
echo "  previous -> $(readlink -f "$PREVIOUS_LINK")"
echo "重启服务: sudo systemctl restart hangzhouwan.target"
