#!/bin/bash
# -*- coding: utf-8 -*-
# 回滚到上一个 git commit。
#
# 用法：
#   bash deploy/rollback.sh [commit_hash]
#   bash deploy/rollback.sh HEAD~1

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

TARGET="${1:-HEAD~1}"

echo "=== 回滚到 $TARGET ==="

# 停止服务
for svc in hangzhouwan-business hangzhouwan-video; do
    if systemctl is-active "$svc.service" >/dev/null 2>&1; then
        sudo systemctl stop "$svc.service"
        echo "  已停止 $svc"
    fi
done

# 回滚代码
CURRENT=$(git rev-parse --short HEAD)
git stash 2>/dev/null || true
git checkout "$TARGET"
NEW=$(git rev-parse --short HEAD)
echo "  代码回滚: $CURRENT -> $NEW"

# 重新构建（如果需要）
if [ -f CMakeLists.txt ]; then
    echo "  重新构建..."
    cd build && cmake .. && make -j4 && cd ..
fi

echo "=== 回滚完成 ==="
echo "请手动重启服务："
echo "  sudo systemctl start hangzhouwan-business.service"
echo "  sudo systemctl start hangzhouwan-video.service"
