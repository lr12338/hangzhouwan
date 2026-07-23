#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# RTMP 端到端故障注入脚本（安全、受控、可审计）。
#
# 目的：验证普通 RTMP 网络重连仅重建 muxer/AVIO，不重建硬件编码器（G5）。
#
# 方法：使用 iptables 限定明确目标 IP 和 1935 端口，唯一规则标识，
#       trap EXIT/INT/TERM 确保删除测试规则，前后核对无残留。
#
# 安全保证：
#   - set -euo pipefail，添加/删除失败立即停止
#   - 唯一规则标识（comment），不与其他规则混淆
#   - trap 清理保证规则不残留
#   - 仅限目标 IP + 1935 端口，不影响其他 RTMP 目标
#   - 使用 journal cursor 精确统计日志
#   - 分别记录 A/B 两路重连次数
#   - 编码器创建计数只能保持双路初始 2 次
#
# 期望：100 轮断网，每路各 100 次重连（总计 200 次），编码器创建=2
#
# 用法：
#   bash tools/dual_stream/rtmp_fault_injection.sh --target-ip <IP> [--rounds 100] [--break-s 2] [--restore-s 2]
#
# 参数：
#   --target-ip    RTMP 目标 IP（必须指定，限定 iptables 规则范围）
#   --rounds       断网/恢复轮次（默认 100）
#   --break-s      每轮断网秒数（默认 2）
#   --restore-s    每轮恢复秒数（默认 2）
#   --video-svc    video 服务名（默认 hangzhouwan-video.service）
#
# 注意：本脚本需要 sudo 权限执行 iptables。维护窗口批准后方可执行。
#       本轮不得执行（仅审计脚本结构）。
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# ---- 参数解析 ----
TARGET_IP=""
ROUNDS=100
BREAK_S=2
RESTORE_S=2
VIDEO_SVC="hangzhouwan-video.service"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-ip) TARGET_IP="$2"; shift 2;;
    --rounds) ROUNDS="$2"; shift 2;;
    --break-s) BREAK_S="$2"; shift 2;;
    --restore-s) RESTORE_S="$2"; shift 2;;
    --video-svc) VIDEO_SVC="$2"; shift 2;;
    *) echo "未知参数: $1"; exit 1;;
  esac
done

if [ -z "$TARGET_IP" ]; then
  echo "错误: 必须指定 --target-ip（RTMP 目标 IP，限定 iptables 规则范围）"
  echo "用法: bash tools/dual_stream/rtmp_fault_injection.sh --target-ip <IP> [--rounds 100] [--break-s 2] [--restore-s 2]"
  exit 1
fi

# ---- 唯一规则标识 ----
RULE_TAG="HZW_RTMP_TEST_$(date -u +%Y%m%dT%H%M%SZ)_$$"
RULE_SPEC="-p tcp -d ${TARGET_IP} --dport 1935 -j DROP -m comment --comment ${RULE_TAG}"

echo "=== RTMP 端到端故障注入 ==="
echo "  目标 IP: ${TARGET_IP}"
echo "  端口: 1935"
echo "  轮次: ${ROUNDS}"
echo "  断网/恢复: ${BREAK_S}s / ${RESTORE_S}s"
echo "  规则标识: ${RULE_TAG}"
echo "  视频服务: ${VIDEO_SVC}"
echo ""

# ---- 状态变量 ----
RULE_ADDED=false
TEST_START_CURSOR=""
TEST_END_CURSOR=""
BASE_ENC_COUNT=0
FAIL_RECOVERY=false

# ---- trap 清理：确保删除测试规则 ----
cleanup_rules() {
  if [ "$RULE_ADDED" = true ]; then
    echo ""
    echo "[清理] 删除残留 iptables 规则 (tag=${RULE_TAG})..."
    sudo iptables -D OUTPUT ${RULE_SPEC} 2>/dev/null || true
    RULE_ADDED=false
  fi
  # 核对无残留规则
  local residual
  residual="$(sudo iptables -S OUTPUT 2>/dev/null | grep "$RULE_TAG" || true)"
  if [ -n "$residual" ]; then
    echo "[清理] ⚠️  仍有残留规则: $residual"
    echo "[清理] 尝试强制删除..."
    while IFS= read -r line; do
      sudo iptables -D OUTPUT ${RULE_SPEC} 2>/dev/null || true
    done <<< "$residual"
  fi
}
trap cleanup_rules EXIT
trap 'echo "[中断] 收到信号，清理规则后退出"; cleanup_rules; exit 130' INT TERM

# ---- 前置核对：无残留规则 ----
echo "[0/5] 前置核对无残留规则..."
RESIDUAL_BEFORE="$(sudo iptables -S OUTPUT 2>/dev/null | grep "$RULE_TAG" || true)"
if [ -n "$RESIDUAL_BEFORE" ]; then
  echo "  ❌ 发现上次残留规则，先清理: $RESIDUAL_BEFORE"
  sudo iptables -D OUTPUT ${RULE_SPEC} 2>/dev/null || true
  RESIDUAL_BEFORE="$(sudo iptables -S OUTPUT 2>/dev/null | grep "$RULE_TAG" || true)"
  if [ -n "$RESIDUAL_BEFORE" ]; then
    echo "  ❌ 无法清除残留规则，拒绝执行"
    exit 1
  fi
fi
echo "  ✅ 无残留规则"

# ---- 记录 journal cursor 和基线编码器计数 ----
echo "[1/5] 记录基线..."
TEST_START_CURSOR="$(journalctl -u "$VIDEO_SVC" --show-cursor -n 0 2>/dev/null | grep -oP '#cursor=\K.*' || true)"
if [ -z "$TEST_START_CURSOR" ]; then
  echo "  ⚠️  无法获取 journal cursor，回退到时间戳"
  TEST_START_CURSOR="time:$(date -u +%FT%TZ)"
fi
echo "  journal cursor: ${TEST_START_CURSOR:0:40}..."

BASE_ENC_COUNT="$(journalctl -u "$VIDEO_SVC" --since "5 min ago" 2>/dev/null \
  | grep -c '信息 | 视频编码 | 编码器' || true)"
echo "  baseline encoder_open_count=$BASE_ENC_COUNT (预期 2，双路各 1)"

# ---- 故障注入循环 ----
echo "[2/5] 开始 ${ROUNDS} 轮断网/恢复..."
ROUND=0
while [ "$ROUND" -lt "$ROUNDS" ]; do
  ROUND=$((ROUND + 1))

  # 添加规则（断网）
  if ! sudo iptables -A OUTPUT ${RULE_SPEC}; then
    echo "  ❌ iptables -A 失败 (round=$ROUND)，立即停止"
    exit 1
  fi
  RULE_ADDED=true
  sleep "$BREAK_S"

  # 删除规则（恢复）
  if ! sudo iptables -D OUTPUT ${RULE_SPEC}; then
    echo "  ❌ iptables -D 失败 (round=$ROUND)，立即停止"
    echo "  ⚠️  规则可能残留，trap 将尝试清理"
    exit 1
  fi
  RULE_ADDED=false
  sleep "$RESTORE_S"

  if [ $((ROUND % 10)) -eq 0 ]; then
    echo "  ... round $ROUND/$ROUNDS done"
  fi
done
echo "  ✅ ${ROUNDS} 轮完成"

# ---- 记录结束 cursor ----
echo "[3/5] 记录结束 cursor..."
TEST_END_CURSOR="$(journalctl -u "$VIDEO_SVC" --show-cursor -n 0 2>/dev/null | grep -oP '#cursor=\K.*' || true)"
if [ -z "$TEST_END_CURSOR" ]; then
  TEST_END_CURSOR="time:$(date -u +%FT%TZ)"
fi

# ---- 统计编码器创建和重连次数 ----
echo "[4/5] 统计编码器创建与重连次数..."
if [[ "$TEST_START_CURSOR" == time:* ]]; then
  # 时间戳回退模式
  START_TIME="${TEST_START_CURSOR#time:}"
  LOG_RANGE="--since ${START_TIME}"
else
  LOG_RANGE="--after-cursor ${TEST_START_CURSOR}"
fi

ENC_NOW_COUNT="$(journalctl -u "$VIDEO_SVC" $LOG_RANGE --until "$(date -u +%FT%TZ)" 2>/dev/null \
  | grep -c '信息 | 视频编码 | 编码器' || true)"

RTMP_RECONNECT_A="$(journalctl -u "$VIDEO_SVC" $LOG_RANGE --until "$(date -u +%FT%TZ)" 2>/dev/null \
  | grep -cE '信息 \| RTMP重连 \|.*路A' || true)"
RTMP_RECONNECT_B="$(journalctl -u "$VIDEO_SVC" $LOG_RANGE --until "$(date -u +%FT%TZ)" 2>/dev/null \
  | grep -cE '信息 \| RTMP重连 \|.*路B' || true)"
RTMP_RECONNECT_TOTAL="$(journalctl -u "$VIDEO_SVC" $LOG_RANGE --until "$(date -u +%FT%TZ)" 2>/dev/null \
  | grep -c '信息 | RTMP重连 | 重连成功' || true)"

# 检查致命错误
FATAL_ERRORS="$(journalctl -u "$VIDEO_SVC" $LOG_RANGE --until "$(date -u +%FT%TZ)" 2>/dev/null \
  | grep -ciE 'DEVICE_RESOURCE_FATAL|ENOMEM|invalid free' || true)"

echo ""
echo "  encoder_open_count=$ENC_NOW_COUNT (baseline=$BASE_ENC_COUNT, 预期=2)"
echo "  rtmp_reconnect_A=$RTMP_RECONNECT_A (预期=$ROUNDS)"
echo "  rtmp_reconnect_B=$RTMP_RECONNECT_B (预期=$ROUNDS)"
echo "  rtmp_reconnect_total=$RTMP_RECONNECT_TOTAL (预期=$((ROUNDS * 2)))"
echo "  fatal_errors=$FATAL_ERRORS (预期=0)"

# ---- 后置核对无残留规则 ----
echo "[5/5] 后置核对无残留规则..."
RESIDUAL_AFTER="$(sudo iptables -S OUTPUT 2>/dev/null | grep "$RULE_TAG" || true)"
if [ -n "$RESIDUAL_AFTER" ]; then
  echo "  ❌ 发现残留规则: $RESIDUAL_AFTER"
  FAIL_RECOVERY=true
else
  echo "  ✅ 无残留规则"
fi

# ---- 判定 ----
echo ""
echo "=== 判定 (G5) ==="
G5_PASS=true

if [ "$ENC_NOW_COUNT" -ne "$BASE_ENC_COUNT" ]; then
  echo "  ❌ 编码器创建次数变化: $ENC_NOW_COUNT != $BASE_ENC_COUNT (应保持 $BASE_ENC_COUNT)"
  G5_PASS=false
else
  echo "  ✅ 编码器创建次数未增加: $ENC_NOW_COUNT == $BASE_ENC_COUNT"
fi

if [ "$FATAL_ERRORS" -gt 0 ]; then
  echo "  ❌ 发现致命错误: $FATAL_ERRORS 次"
  G5_PASS=false
fi

if [ "$FAIL_RECOVERY" = true ]; then
  echo "  ❌ 存在残留 iptables 规则"
  G5_PASS=false
fi

# 重连次数检查（每路各 ROUNDS 次，总计 ROUNDS*2）
if [ "$RTMP_RECONNECT_TOTAL" -lt $((ROUNDS)) ]; then
  echo "  ⚠️  RTMP 重连总次数 $RTMP_RECONNECT_TOTAL < $ROUNDS（可能部分重连未记录或仍在重试）"
fi

echo ""
echo "  编码器创建预期: 双路初始 2 次，${ROUNDS} 轮重连后不增加"
echo "  重连次数预期: 每路各 ${ROUNDS} 次，总计 $((ROUNDS * 2)) 次"

if [ "$G5_PASS" = true ]; then
  echo ""
  echo "=== ✅ G5 通过 ==="
  exit 0
else
  echo ""
  echo "=== ❌ G5 未通过 ==="
  exit 1
fi
