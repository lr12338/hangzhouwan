#!/bin/bash
# -*- coding: utf-8 -*-
# 安装服务（通过 Release 制品，不自动 enable）。
#
# 用法：
#   sudo bash deploy/install.sh [release_dir]
#
# 如果未指定 release_dir，使用 /opt/hangzhouwan/current。

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_DIR="${1:-/opt/hangzhouwan/current}"
exec bash "$REPO_ROOT/tools/release/install_release.sh" "$RELEASE_DIR"
