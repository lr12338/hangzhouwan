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
#   2. candidate preflight（--offline --activation）- hzwctl 缺失硬失败，不跳过
#   3. candidate 离线 smoke（VERSION 可读、dual_stream_app 可执行）
#   4. 创建 current.new -> candidate
#   5. mv -T 原子替换 current（保留 previous）
#   6. restart hangzhouwan.target - 失败硬失败并进入受控回滚
#   7. wait-business
#   8. wait-video
#   9. 运行 60 秒 smoke
#  10. PID 核验：实际运行 PID 的可执行文件来自候选 Release
#  11. 失败时自动切回 previous，重启 previous，验证 readiness + health
#  12. 输出明确结果码（自动回滚失败输出独立严重错误码 99）
#
# 结果码：
#   0   成功
#   1   用法错误
#   2   verify_release 失败
#   3   hzwctl 缺失（硬失败，不跳过预检）
#   4   preflight 未通过
#   5   Business 未就绪
#   6   Video 未就绪
#   7   smoke 失败
#   8   PID 核验失败（运行的可执行文件不来自候选 Release）
#   50  systemctl restart 失败
#   90  自动回滚成功但 health 未达标
#   99  自动回滚失败（严重，需人工介入）
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
TARGET_SVC="hangzhouwan.target"
RESULT_CODE=0

# ---------------------------------------------------------------------------
# 自动回滚函数（必须在调用前定义）
# ---------------------------------------------------------------------------
perform_rollback() {
  echo ""
  echo "=== ❌ 激活失败（结果码=$RESULT_CODE），自动回滚 ==="
  if [ ! -L "$PREVIOUS_LINK" ]; then
    echo "  ❌ 无 previous 可回滚 - 严重错误，需人工介入"
    echo "  当前 current -> $(readlink -f "$CURRENT_LINK" 2>/dev/null || echo '未知')"
    RESULT_CODE=99
    return
  fi

  local prev_target
  prev_target="$(readlink -f "$PREVIOUS_LINK")"
  if [ -z "$prev_target" ] || [ ! -d "$prev_target" ]; then
    echo "  ❌ previous 指向无效目录: $prev_target - 严重错误"
    RESULT_CODE=99
    return
  fi

  echo "  回滚到 previous: $prev_target"
  if ! sudo ln -sfn "$prev_target" "$CURRENT_LINK"; then
    echo "  ❌ 无法恢复 current 符号链接"
    RESULT_CODE=99
    return
  fi
  echo "  current -> $prev_target"

  # 重启 previous（先 reset-failed 清除 StartLimitBurst）
  echo "  重启 previous..."
  sudo systemctl reset-failed "$TARGET_SVC" 2>/dev/null || true
  sudo systemctl reset-failed hangzhouwan-business.service 2>/dev/null || true
  sudo systemctl reset-failed hangzhouwan-video.service 2>/dev/null || true
  if ! sudo systemctl restart "$TARGET_SVC"; then
    echo "  ❌ 回滚后 systemctl restart 失败 - 严重错误，需人工介入"
    RESULT_CODE=99
    return
  fi

  # 等待 Business + Video 就绪
  sleep 5
  local rollback_ok=true health_status=""
  if [ -x "${CURRENT_LINK}/bin/hzwctl" ]; then
    if ! sudo "${CURRENT_LINK}/bin/hzwctl" wait-business --timeout 30 2>/dev/null; then
      echo "  ❌ 回滚后 Business 未就绪"
      rollback_ok=false
    else
      echo "  ✅ 回滚后 Business 就绪"
    fi
    if [ "$rollback_ok" = true ]; then
      if ! sudo "${CURRENT_LINK}/bin/hzwctl" wait-video --timeout 60 2>/dev/null; then
        echo "  ❌ 回滚后 Video 未就绪"
        rollback_ok=false
      else
        echo "  ✅ 回滚后 Video 就绪"
      fi
    fi
    # wait-video 只保证 socket/进程可用，旧 Release 此时可能仍处于 STARTING。
    # 最多再等待 90 秒进入稳定状态，避免把一次正常冷启动误报为回滚失败。
    if [ "$rollback_ok" = true ]; then
      local health_output health_deadline
      health_deadline=$((SECONDS + 90))
      while [ $SECONDS -lt $health_deadline ]; do
        # health --json 在 DEGRADED/FAILED 时会以非零码退出，但 JSON 本身仍
        # 是有效结果；不可用 `cmd || fallback` 拼接两份输出，否则 json.loads
        # 会因 trailing data 失败并把真实 DEGRADED 误判为空状态。
        health_output="$(sudo "${CURRENT_LINK}/bin/hzwctl" health --json \
          2>/dev/null || true)"
        if [ -z "$health_output" ]; then
          health_output="$(sudo "${CURRENT_LINK}/bin/hzwctl" health \
            2>/dev/null || true)"
        fi
        health_status="$(printf '%s' "$health_output" | python3 -c "
import json, sys
try:
    raw = sys.stdin.read()
    d = json.loads(raw[raw.find('{'):])
    print(d.get('status', ''))
except Exception:
    print('')
" 2>/dev/null || echo '')"
        if [ "$health_status" = "HEALTHY" ] ||
           [ "$health_status" = "DEGRADED" ]; then
          break
        fi
        sleep 2
      done
      if [ "$health_status" = "HEALTHY" ] ||
         [ "$health_status" = "DEGRADED" ]; then
        echo "  ✅ 回滚后 health=$health_status"
      else
        echo "  ⚠️  回滚后等待 90 秒 health=$health_status（非 HEALTHY/DEGRADED）"
        rollback_ok=false
      fi
    fi
  else
    echo "  ⚠️  current/bin/hzwctl 不可用，无法验证 readiness/health"
    rollback_ok=false
  fi

  # 核验 current/previous 指向
  local cur_now prev_now
  cur_now="$(readlink -f "$CURRENT_LINK" 2>/dev/null || true)"
  prev_now="$(readlink -f "$PREVIOUS_LINK" 2>/dev/null || true)"
  echo "  核验指向: current -> $cur_now | previous -> $prev_now"
  if [ "$cur_now" != "$prev_target" ]; then
    echo "  ❌ 回滚后 current 未指向 previous - 严重错误"
    rollback_ok=false
  fi

  if [ "$rollback_ok" = true ]; then
    echo ""
    if [ "$health_status" = "HEALTHY" ]; then
      echo "=== 回滚完成（health=HEALTHY）==="
      # 回滚成功但原激活失败，保留原 RESULT_CODE
    else
      echo "=== 回滚完成但 health 未达 HEALTHY（结果码=90）==="
      RESULT_CODE=90
    fi
  else
    echo ""
    echo "=== ❌❌ 自动回滚失败（结果码=99）- 严重错误，需人工介入 ==="
    echo "  current -> $cur_now"
    echo "  previous -> $prev_now"
    echo "  请手动检查: systemctl status $TARGET_SVC"
    echo "  请手动检查: pgrep -x dual_stream_app"
    echo "  请手动检查: bm-smi"
    RESULT_CODE=99
  fi
}

# ===========================================================================
# 主流程
# ===========================================================================
echo "=== 激活 Release: ${RELEASE_DIR} ==="
echo "  Config: ${CONFIG_PATH}"
echo ""

# ---------------------------------------------------------------------------
# 1. verify candidate
# ---------------------------------------------------------------------------
echo "[1/10] 验证 Release 完整性..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if ! bash "${SCRIPT_DIR}/verify_release.sh" "$RELEASE_DIR" >/dev/null 2>&1; then
  echo "  ❌ Release 验证失败"
  exit 2
fi
echo "  ✅ Release 验证通过"

# ---------------------------------------------------------------------------
# 2. candidate preflight（离线 + 激活条件）- hzwctl 缺失硬失败
# ---------------------------------------------------------------------------
echo "[2/10] 候选 Release 预检..."
if [ ! -x "$HZWCTL" ]; then
  echo "  ❌ hzwctl 不可用: $HZWCTL"
  echo "  hzwctl 缺失必须硬失败，不跳过预检"
  exit 3
fi
if ! sudo "$HZWCTL" preflight --release "$RELEASE_DIR" --config "$CONFIG_PATH" --offline --activation; then
  echo "  ❌ 预检未通过，拒绝激活"
  exit 4
fi
echo "  ✅ 预检通过"

# ---------------------------------------------------------------------------
# 3. candidate 离线 smoke
# ---------------------------------------------------------------------------
echo "[3/10] 候选 Release 离线 smoke..."
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
echo "[4/10] 准备原子切换..."
sudo mkdir -p "$BASE_DIR"

if [ -L "$CURRENT_LINK" ]; then
  OLD_TARGET="$(readlink -f "$CURRENT_LINK")"
  echo "  当前 current -> $OLD_TARGET"
fi

CURRENT_NEW="${BASE_DIR}/.current.new.$$"
if ! sudo ln -sfn "$RELEASE_DIR" "$CURRENT_NEW"; then
  echo "  ❌ 无法创建候选 current 链接"
  exit 4
fi

echo "[5/10] 原子替换 current..."
if [ -L "$CURRENT_LINK" ]; then
  OLD_TARGET="$(readlink -f "$CURRENT_LINK")"
  if ! sudo ln -sfn "$OLD_TARGET" "$PREVIOUS_LINK"; then
    echo "  ❌ 无法更新 previous 链接"
    sudo unlink "$CURRENT_NEW" 2>/dev/null || true
    exit 4
  fi
  echo "  previous -> $OLD_TARGET"
fi

if ! sudo mv -T "$CURRENT_NEW" "$CURRENT_LINK"; then
  echo "  ❌ 原子替换 current 失败，current 保持原状"
  sudo unlink "$CURRENT_NEW" 2>/dev/null || true
  exit 4
fi
echo "  current -> $RELEASE_DIR"

# ---------------------------------------------------------------------------
# 6. restart target - 失败硬失败并进入受控回滚
# ---------------------------------------------------------------------------
echo "[6/10] 重启 hangzhouwan.target..."
if ! sudo systemctl restart "$TARGET_SVC"; then
  echo "  ❌ systemctl restart 失败，进入受控回滚"
  RESULT_CODE=50
  perform_rollback
  exit "$RESULT_CODE"
fi
echo "  ✅ systemctl restart 成功"

# ---------------------------------------------------------------------------
# 7. wait-business
# ---------------------------------------------------------------------------
HZWCTL_CURRENT="${CURRENT_LINK}/bin/hzwctl"
echo "[7/10] 等待 Business 就绪..."
if [ ! -x "$HZWCTL_CURRENT" ]; then
  echo "  ❌ current/bin/hzwctl 不可用"
  RESULT_CODE=5
  perform_rollback
  exit "$RESULT_CODE"
fi
if ! sudo "$HZWCTL_CURRENT" wait-business --timeout 30; then
  echo "  ❌ Business 未就绪"
  RESULT_CODE=5
  perform_rollback
  exit "$RESULT_CODE"
fi
echo "  ✅ Business 就绪"

# ---------------------------------------------------------------------------
# 8. 严格 wait-health
# ---------------------------------------------------------------------------
echo "[8/10] 等待 A/B 全链路严格健康..."
if ! sudo "$HZWCTL_CURRENT" wait-health --timeout 180; then
  echo "  ❌ 严格健康门禁未通过"
  RESULT_CODE=6
  perform_rollback
  exit "$RESULT_CODE"
fi
echo "  ✅ A/B 全链路健康"

# ---------------------------------------------------------------------------
# 9. 60 秒 smoke
# ---------------------------------------------------------------------------
if [ "$NO_SMOKE" = false ]; then
  echo "[9/10] 运行 60 秒 smoke..."
  SMOKE_PASS=true
  SMOKE_END=$((SECONDS + 60))
  while [ $SECONDS -lt $SMOKE_END ]; do
    sleep 5
    if ! sudo "$HZWCTL_CURRENT" smoke-test >/dev/null 2>&1; then
      echo "  ⚠️  smoke 检查失败"
      SMOKE_PASS=false
      break
    fi
  done
  if [ "$SMOKE_PASS" = false ]; then
    echo "  ❌ 60s smoke 失败"
    RESULT_CODE=7
    perform_rollback
    exit "$RESULT_CODE"
  else
    echo "  ✅ 60s smoke 通过"
  fi
else
  echo "[9/10] 跳过 smoke（--no-smoke）"
fi

# ---------------------------------------------------------------------------
# 10. PID 核验：实际运行 PID 的可执行文件来自候选 Release
# ---------------------------------------------------------------------------
echo "[10/10] PID 核验（可执行文件来源）..."
CURRENT_REAL="$(readlink -f "$CURRENT_LINK")"
PID_FOUND=false
PID_EXE_OK=false
while IFS= read -r pid; do
  [ -z "$pid" ] && continue
  PID_FOUND=true
  # Video 以独立生产账户运行；普通维护用户在 hidepid/Yama 等加固下
  # 无权解析 /proc/<pid>/exe。这里只提升只读核验动作，避免权限拒绝
  # 被误判为“进程不来自候选 Release”。
  EXE_PATH="$(sudo readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
  if [ -n "$EXE_PATH" ]; then
    case "$EXE_PATH" in
      "$CURRENT_REAL"/*)
        PID_EXE_OK=true
        echo "  PID $pid: $EXE_PATH ✅ 来自候选 Release"
        ;;
      *)
        echo "  PID $pid: $EXE_PATH ❌ 不来自候选 Release ($CURRENT_REAL)"
        ;;
    esac
  fi
done < <(pgrep -x dual_stream_app 2>/dev/null || true)

if [ "$PID_FOUND" = false ]; then
  echo "  ❌ 未找到 dual_stream_app 进程"
  RESULT_CODE=8
  perform_rollback
  exit "$RESULT_CODE"
fi
if [ "$PID_EXE_OK" = false ]; then
  echo "  ❌ 运行的可执行文件不来自候选 Release"
  RESULT_CODE=8
  perform_rollback
  exit "$RESULT_CODE"
fi
echo "  ✅ PID 核验通过"

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
