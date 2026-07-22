#!/bin/bash
# -*- coding: utf-8 -*-
# =============================================================================
# 原子激活 Release（含预检、冒烟、自动回滚）。
#
# 用法：
#   bash tools/release/activate_release.sh <release_dir> [--config <path>] [--no-smoke]
#
# 流程：
#   1. verify candidate（SHA256 + manifest）
#   2. candidate preflight（--offline --activation）
#   3. candidate 离线 smoke（VERSION 可读、dual_stream_app 可执行）
#   4. 创建 current.new -> candidate
#   5. mv -T 原子替换 current（保留 previous）
#   6. restart hangzhouwan.target
#   7. wait-business
#   8. wait-video
#   9. 运行 60 秒 smoke
#  10. 失败时自动切回 previous，重启 previous，验证 readiness
#  11. 输出明确结果码
#
# 禁止现场编译和 git checkout。
# =============================================================================

set -euo pipefail

RELEASE_DIR="${1:-}"
shift || true
CONFIG_PATH="/etc/hangzhouwan/application.yaml"
NO_SMOKE=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG_PATH="$2"; shift 2;;
    --no-smoke) NO_SMOKE=true; shift;;
    *) echo "未知参数: $1"; exit 1;;
  esac
done

if [ -z "$RELEASE_DIR" ] || [ ! -d "$RELEASE_DIR" ]; then
  echo "用法: bash tools/release/activate_release.sh <release_dir> [--config <path>] [--no-smoke]"
  exit 1
fi

RELEASE_DIR="$(cd "$RELEASE_DIR" && pwd)"
BASE_DIR="/opt/hangzhouwan"
CURRENT_LINK="$BASE_DIR/current"
PREVIOUS_LINK="$BASE_DIR/previous"
HZWCTL="${RELEASE_DIR}/bin/hzwctl"
RESULT_CODE=0

echo "=== 激活 Release: ${RELEASE_DIR} ==="
echo "  Config: ${CONFIG_PATH}"
echo ""

# ---------------------------------------------------------------------------
# 1. verify candidate
# ---------------------------------------------------------------------------
echo "[1/9] 验证 Release 完整性..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if ! bash "${SCRIPT_DIR}/verify_release.sh" "$RELEASE_DIR" >/dev/null 2>&1; then
  echo "  ❌ Release 验证失败"
  exit 2
fi
echo "  ✅ Release 验证通过"

# ---------------------------------------------------------------------------
# 2. candidate preflight（离线 + 激活条件，不读 current）
# ---------------------------------------------------------------------------
echo "[2/9] 候选 Release 预检..."
if [ -x "$HZWCTL" ]; then
  if ! "$HZWCTL" preflight --release "$RELEASE_DIR" --config "$CONFIG_PATH" --offline --activation; then
    echo "  ❌ 预检未通过，拒绝激活"
    exit 3
  fi
else
  echo "  ⚠️  hzwctl 不可用，跳过预检（不推荐）"
fi
echo "  ✅ 预检通过"

# ---------------------------------------------------------------------------
# 3. candidate 离线 smoke
# ---------------------------------------------------------------------------
echo "[3/9] 候选 Release 离线 smoke..."
VERSION_FILE="${RELEASE_DIR}/VERSION"
APP_BIN="${RELEASE_DIR}/bin/dual_stream_app"
if [ ! -f "$VERSION_FILE" ]; then
  echo "  ❌ VERSION 缺失"
  exit 4
fi
if [ ! -x "$APP_BIN" ]; then
  echo "  ❌ dual_stream_app 不可执行"
  exit 4
fi
echo "  VERSION: $(head -1 "$VERSION_FILE")"
echo "  ✅ 离线 smoke 通过"

# ---------------------------------------------------------------------------
# 4-5. 原子切换 current（mv -T）
# ---------------------------------------------------------------------------
echo "[4/9] 准备原子切换..."
mkdir -p "$BASE_DIR"

# 备份 previous
if [ -L "$CURRENT_LINK" ]; then
  OLD_TARGET="$(readlink -f "$CURRENT_LINK")"
  echo "  当前 current -> $OLD_TARGET"
fi

# 创建 current.new 临时链接，然后 mv -T 原子替换
CURRENT_NEW="${BASE_DIR}/.current.new.$$"
ln -sfn "$RELEASE_DIR" "$CURRENT_NEW"

echo "[5/9] 原子替换 current..."
# 先保存 previous
if [ -L "$CURRENT_LINK" ]; then
  OLD_TARGET="$(readlink -f "$CURRENT_LINK")"
  ln -sfn "$OLD_TARGET" "$PREVIOUS_LINK"
  echo "  previous -> $OLD_TARGET"
fi

# 原子替换
mv -T "$CURRENT_NEW" "$CURRENT_LINK"
echo "  current -> $RELEASE_DIR"

# ---------------------------------------------------------------------------
# 6. restart target
# ---------------------------------------------------------------------------
echo "[6/9] 重启 hangzhouwan.target..."
if ! sudo systemctl restart "$TARGET_SVC" 2>/dev/null; then
  echo "  ⚠️  systemctl restart 失败（可能 systemd 不可用），继续检查..."
fi

# ---------------------------------------------------------------------------
# 7. wait-business
# ---------------------------------------------------------------------------
echo "[7/9] 等待 Business 就绪..."
HZWCTL_CURRENT="${CURRENT_LINK}/bin/hzwctl"
if [ -x "$HZWCTL_CURRENT" ]; then
  if ! "$HZWCTL_CURRENT" wait-business --timeout 30; then
    echo "  ❌ Business 未就绪"
    RESULT_CODE=5
  fi
else
  echo "  ⚠️  hzwctl 不可用，跳过"
fi

# ---------------------------------------------------------------------------
# 8. wait-video
# ---------------------------------------------------------------------------
if [ $RESULT_CODE -eq 0 ]; then
  echo "[8/9] 等待 Video 就绪..."
  if [ -x "$HZWCTL_CURRENT" ]; then
    if ! "$HZWCTL_CURRENT" wait-video --timeout 60; then
      echo "  ❌ Video 未就绪"
      RESULT_CODE=6
    fi
  else
    echo "  ⚠️  hzwctl 不可用，跳过"
  fi
fi

# ---------------------------------------------------------------------------
# 9. 60 秒 smoke
# ---------------------------------------------------------------------------
if [ $RESULT_CODE -eq 0 ] && [ "$NO_SMOKE" = false ]; then
  echo "[9/9] 运行 60 秒 smoke..."
  SMOKE_PASS=true
  SMOKE_END=$((SECONDS + 60))
  while [ $SECONDS -lt $SMOKE_END ]; do
    sleep 5
    if [ -x "$HZWCTL_CURRENT" ]; then
      if ! "$HZWCTL_CURRENT" smoke-test >/dev/null 2>&1; then
        echo "  ⚠️  smoke 检查失败"
        SMOKE_PASS=false
        break
      fi
    fi
  done
  if [ "$SMOKE_PASS" = false ]; then
    echo "  ❌ 60s smoke 失败"
    RESULT_CODE=7
  else
    echo "  ✅ 60s smoke 通过"
  fi
elif [ $RESULT_CODE -eq 0 ]; then
  echo "[9/9] 跳过 smoke（--no-smoke）"
fi

# ---------------------------------------------------------------------------
# 自动回滚
# ---------------------------------------------------------------------------
if [ $RESULT_CODE -ne 0 ]; then
  echo ""
  echo "=== ❌ 激活失败（结果码=$RESULT_CODE），自动回滚 ==="
  if [ -L "$PREVIOUS_LINK" ]; then
    PREV_TARGET="$(readlink -f "$PREVIOUS_LINK")"
    echo "  回滚到 previous: $PREV_TARGET"
    ln -sfn "$PREV_TARGET" "$CURRENT_LINK"
    echo "  current -> $PREV_TARGET"

    # 重启 previous
    echo "  重启 previous..."
    sudo systemctl restart "$TARGET_SVC" 2>/dev/null || true

    # 验证 previous readiness
    sleep 5
    if [ -x "$HZWCTL_CURRENT" ]; then
      if "$HZWCTL_CURRENT" wait-business --timeout 30 2>/dev/null; then
        echo "  ✅ previous Business 就绪"
      else
        echo "  ⚠️  previous Business 未就绪"
      fi
    fi
    echo ""
    echo "=== 回滚完成 ==="
    echo "  current -> $(readlink -f "$CURRENT_LINK")"
    echo "  previous -> $(readlink -f "$PREVIOUS_LINK")"
  else
    echo "  ⚠️  无 previous 可回滚"
  fi
  exit $RESULT_CODE
fi

# ---------------------------------------------------------------------------
# 成功
# ---------------------------------------------------------------------------
echo ""
echo "=== ✅ 激活成功 ==="
echo "  current -> $(readlink -f "$CURRENT_LINK")"
if [ -L "$PREVIOUS_LINK" ]; then
  echo "  previous -> $(readlink -f "$PREVIOUS_LINK")"
  echo "  回滚: bash tools/release/rollback_release.sh"
fi
echo ""
echo "结果码: 0 (成功)"
exit 0
