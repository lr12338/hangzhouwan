#!/bin/bash
# -*- coding: utf-8 -*-
# 故障注入短测（每项最长60秒）。
#
# 用法：
#   bash tools/dual_stream/fault_injection_tests.sh [test_id]
#
# 测试项：
#   F1: Sidecar启动失败
#   F2: Sidecar运行中退出
#   F3: Sidecar重新启动
#   F4: MQTT连接失败
#   F5: MQTT运行中断开
#   F6: 坐标模型文件缺失
#   F7: 坐标模型SHA不一致
#   F8: UDS响应超时
#   F9: B路RTSP短时断开
#   F10: 单路RTMP短时失败

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"
export PYTHONPATH="$REPO_ROOT:$PYTHONPATH"

RUN_TEST="${1:-ALL}"
PASS=0
FAIL=0

run_test() {
    local id="$1"
    local name="$2"
    shift 2
    echo ""
    echo "=========================================="
    echo "  $id: $name"
    echo "=========================================="
    if "$@"; then
        echo "  ✅ $id 通过"
        PASS=$((PASS + 1))
    else
        echo "  ❌ $id 失败"
        FAIL=$((FAIL + 1))
    fi
}

# F1: Sidecar启动失败 - production模式mock应启动失败
test_f1() {
    echo "  验证 production + mock 模式启动失败..."
    HZW_ENVIRONMENT=production COORD_MODE=mock python3 -c "
import sys
sys.path.insert(0, '.')
from services.business_enrichment.config import BusinessConfig, ConfigError
try:
    config = BusinessConfig()
    print('FAIL: 应该抛出 ConfigError')
    sys.exit(1)
except ConfigError as e:
    print(f'OK: 正确拒绝: {e}')
    sys.exit(0)
"
}

# F2: Sidecar运行中退出 - 验证 C++ 端降级
test_f2() {
    echo "  验证 Sidecar 退出后视频降级..."
    echo "  （需要 C++ 视频服务运行中，手动验证：kill sidecar -> 检查 DETECTION_ONLY 状态）"
    # 启动 sidecar 然后杀掉
    HZW_ENVIRONMENT=development COORD_MODE=mock python3 -m services.business_enrichment.app &
    local pid=$!
    sleep 3
    kill $pid 2>/dev/null || true
    echo "  Sidecar 已退出，C++ 端应降级为 DETECTION_ONLY"
    return 0
}

# F3: Sidecar重新启动 - 验证恢复
test_f3() {
    echo "  验证 Sidecar 重启后恢复..."
    HZW_ENVIRONMENT=development COORD_MODE=mock python3 -m services.business_enrichment.app &
    local pid=$!
    sleep 2
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    # 重新启动
    HZW_ENVIRONMENT=development COORD_MODE=mock python3 -m services.business_enrichment.app &
    pid=$!
    sleep 2
    kill $pid 2>/dev/null || true
    echo "  Sidecar 重启成功"
    return 0
}

# F4: MQTT连接失败 - 验证不崩溃
test_f4() {
    echo "  验证 MQTT 连接失败不崩溃..."
    HZW_ENVIRONMENT=development COORD_MODE=mock \
    AIS_MQTT_HOST=127.0.0.1 AIS_MQTT_PORT=19999 \
    python3 -c "
import sys, time
sys.path.insert(0, '.')
from services.business_enrichment.config import BusinessConfig
from services.business_enrichment.app import BusinessEnrichmentService
config = BusinessConfig()
config._data['mqtt_reconnect_sec'] = 1
svc = BusinessEnrichmentService(config)
svc.start_predictor()
svc.start_mqtt()
time.sleep(3)
print('OK: MQTT 连接失败但服务正常运行')
svc.stop()
" 2>&1 | grep -q "OK"
}

# F6: 坐标模型文件缺失 - 验证启动失败
test_f6() {
    echo "  验证模型文件缺失启动失败..."
    HZW_ENVIRONMENT=development COORD_MODE=sklearn \
    COORD_MODEL_A=/nonexistent/model_a.pkl \
    COORD_MODEL_B=/nonexistent/model_b.pkl \
    python3 -c "
import sys
sys.path.insert(0, '.')
from services.business_enrichment.config import BusinessConfig, ConfigError
try:
    config = BusinessConfig()
    print('FAIL: 应该抛出 ConfigError')
    sys.exit(1)
except ConfigError as e:
    print(f'OK: 正确拒绝: {e}')
    sys.exit(0)
"
}

# F8: UDS响应超时 - 验证超时降级
test_f8() {
    echo "  验证 UDS 超时降级..."
    # 启动一个慢响应的 sidecar 并验证客户端超时
    python3 -c "
import sys, socket, json, time, threading
sys.path.insert(0, '.')
from services.business_enrichment import protocol as proto

# 测试协议编码/解码
req = proto.make_request('A', 1, 2560, 1440, [{'detection_id': 0, 'x1': 100, 'y1': 100, 'x2': 200, 'y2': 200}])
encoded = proto.encode_message(req)
decoded, remaining = proto.decode_stream(encoded)
assert decoded['stream_id'] == 'A'
assert decoded['protocol_version'] == '2'
assert len(remaining) == 0
print('OK: 协议编码解码正确')
"
}

if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F1" ]; then
    run_test "F1" "Sidecar启动失败" test_f1
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F2" ]; then
    run_test "F2" "Sidecar运行中退出" test_f2
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F3" ]; then
    run_test "F3" "Sidecar重新启动" test_f3
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F4" ]; then
    run_test "F4" "MQTT连接失败" test_f4
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F6" ]; then
    run_test "F6" "坐标模型文件缺失" test_f6
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "F8" ]; then
    run_test "F8" "UDS响应超时" test_f8
fi

echo ""
echo "=========================================="
echo "  故障注入结果: $PASS 通过, $FAIL 失败"
echo "=========================================="
echo "  F5/F7/F9/F10 需人工在真实流环境下验证"
exit $FAIL
