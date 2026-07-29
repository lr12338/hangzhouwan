#!/bin/bash
# -*- coding: utf-8 -*-
# 生产预检（委托 hzwctl preflight）。
#
# 用法：
#   bash deploy/preflight.sh
#
# 如果 hzwctl 不可用，回退到内联检查。

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# 优先使用 hzwctl
for candidate in \
  "/opt/hangzhouwan/current/bin/hzwctl" \
  "$REPO_ROOT/build/hzwctl" \
  "$REPO_ROOT/tools/hzwctl.py"; do
  if [ -x "$candidate" ] || [ -f "$candidate" ]; then
    if [[ "$candidate" == *.py ]]; then
      exec python3 "$candidate" preflight
    else
      exec "$candidate" preflight
    fi
  fi
done

echo "错误: hzwctl 未找到，请先构建 Release" >&2
exit 1
