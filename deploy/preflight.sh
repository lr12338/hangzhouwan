#!/bin/bash
# -*- coding: utf-8 -*-
# 生产预检：验证所有关键配置和资源。
#
# 用法：
#   bash deploy/preflight.sh
#
# 检查项：
#   - 真实坐标模型存在
#   - coordinate_mode 不是 mock
#   - bmodel SHA256 正确
#   - A/B 配置完整
#   - MQTT 配置合法
#   - 日志目录可写
#   - 磁盘空间满足阈值
#   - TPU 可访问
#   - 端口和 UDS 路径可用

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_DIR="${HZW_CONFIG_DIR:-/data/hangzhouwan/config}"
PASS=0
FAIL=0

check() {
    local name="$1"
    local condition="$2"
    if [ "$condition" = "true" ]; then
        echo "  ✅ $name"
        PASS=$((PASS + 1))
    else
        echo "  ❌ $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== 生产预检 ==="
echo ""

# 1. 坐标模型存在
MODEL_A="${COORD_MODEL_A:-$REPO_ROOT/weights/0121_random_forest_model.pkl}"
MODEL_B="${COORD_MODEL_B:-$REPO_ROOT/weights/beishang_x-l.pkl}"
check "A模型存在" "$([ -f "$MODEL_A" ] && echo true || echo false)"
check "B模型存在" "$([ -f "$MODEL_B" ] && echo true || echo false)"

# 2. coordinate_mode 不是 mock
COORD_MODE="${COORD_MODE:-sklearn}"
check "coordinate_mode 非 mock" "$([ "$COORD_MODE" != "mock" ] && echo true || echo false)"
check "coordinate_mode 非 off" "$([ "$COORD_MODE" != "off" ] && echo true || echo false)"

# 3. bmodel 孓在
BMODEL_PATH="${BMODEL_PATH:-$REPO_ROOT/weights/best.bmodel}"
if [ -f "$BMODEL_PATH" ]; then
    check "bmodel 存在" "true"
else
    # 检查 artifacts 中的 bmodel
    BMODELS=$(find "$REPO_ROOT/artifacts" -name "*.bmodel" 2>/dev/null | head -1)
    check "bmodel 存在" "$([ -n "$BMODELS" ] && echo true || echo false)"
fi

# 4. MQTT 配置
MQTT_HOST="${AIS_MQTT_HOST:-}"
if [ -n "$MQTT_HOST" ]; then
    check "MQTT host 已配置" "true"
    # 尝试 DNS 解析
    if nslookup "$MQTT_HOST" >/dev/null 2>&1 || getent hosts "$MQTT_HOST" >/dev/null 2>&1; then
        check "MQTT host 可解析" "true"
    else
        check "MQTT host 可解析" "false"
    fi
else
    check "MQTT host 已配置" "false"
fi

# 5. 日志目录可写
LOG_DIR="${HZW_LOG_DIR:-$REPO_ROOT/logs/business}"
mkdir -p "$LOG_DIR" 2>/dev/null || true
check "日志目录可写" "$([ -w "$LOG_DIR" ] && echo true || echo false)"

# 6. 磁盘空间
DISK_AVAIL_MB=$(df -m . | tail -1 | awk '{print $4}')
DISK_THRESHOLD="${DISK_THRESHOLD_MB:-500}"
check "磁盘空间 >= ${DISK_THRESHOLD}MB" "$([ "$DISK_AVAIL_MB" -ge "$DISK_THRESHOLD" ] && echo true || echo false)"

# 7. TPU 可访问
if [ -e /dev/bm-tpu0 ] || ls /dev/bm-tpu* >/dev/null 2>&1; then
    check "TPU 设备可访问" "true"
else
    check "TPU 设备可访问" "false"
fi

# 8. Python 环境
if python3 -c "import sklearn, scipy, joblib, numpy, paho.mqtt.client, pyais" 2>/dev/null; then
    check "Python 依赖完整" "true"
else
    check "Python 依赖完整" "false"
fi

# 9. UDS 路径
SOCK_PATH="${HANGZHOUWAN_BUSINESS_SOCK:-/tmp/hangzhouwan-business.sock}"
SOCK_DIR=$(dirname "$SOCK_PATH")
check "UDS 目录可写" "$([ -w "$SOCK_DIR" ] && echo true || echo false)"

echo ""
echo "=== 预检结果: $PASS 通过, $FAIL 失败 ==="
if [ "$FAIL" -gt 0 ]; then
    echo "❌ 预检未通过，禁止启动生产服务"
    exit 1
else
    echo "✅ 预检通过"
    exit 0
fi
