#!/bin/bash
# -*- coding: utf-8 -*-
# 回滚到上一个 Release（原子软链接切换，不使用 git checkout，不重新编译）。
#
# 用法：
#   bash deploy/rollback.sh
#
# 委托 tools/release/rollback_release.sh。

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec bash "$REPO_ROOT/tools/release/rollback_release.sh" "$@"
