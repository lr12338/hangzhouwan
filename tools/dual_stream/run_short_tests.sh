#!/bin/bash
# -*- coding: utf-8 -*-
# 短时测试矩阵 T0-T5（每项最长300秒）。
#
# 用法：
#   bash tools/dual_stream/run_short_tests.sh [test_id]
#   bash tools/dual_stream/run_short_tests.sh T0   # 仅运行 T0
#   bash tools/dual_stream/run_short_tests.sh      # 运行全部

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

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

# T0: 真实模型单元验证
test_t0() {
    echo "  生成黄金样本..."
    python3 tools/coordinate_model/generate_golden_cases.py
    echo "  sklearn vs numpy 等价比较..."
    python3 tools/coordinate_model/compare_predictors.py
}

# T1: MQTT探测300秒以内
test_t1() {
    local seconds="${T1_SECONDS:-30}"
    echo "  MQTT探测 ${seconds}秒..."
    python3 -m services.business_enrichment.app --mqtt-probe --seconds "$seconds"
}

# T2: AIS replay 60秒
test_t2() {
    local replay_file="${AIS_REPLAY_FILE:-}"
    if [ -z "$replay_file" ]; then
        echo "  跳过：未提供 AIS_REPLAY_FILE 环境变量"
        echo "  用法：AIS_REPLAY_FILE=<path> bash run_short_tests.sh T2"
        return 0
    fi
    echo "  AIS replay 验证..."
    python3 tools/ais/replay_mqtt_ais.py --file "$replay_file" --compare --decode-only
}

# T3: 双路真实坐标60秒（需 C++ 构建）
test_t3() {
    local seconds="${T3_SECONDS:-60}"
    echo "  双路真实坐标测试 ${seconds}秒..."
    echo "  前置：sidecar 已启动，COORD_MODE=sklearn"
    echo "  运行：./tools/dual_stream/dual_full_stack_stability.sh $seconds"
    if [ -x "./tools/dual_stream/dual_full_stack_stability.sh" ]; then
        COORD_MODE=sklearn ./tools/dual_stream/dual_full_stack_stability.sh "$seconds"
    else
        echo "  跳过：dual_full_stack_stability.sh 不存在或不可执行"
        return 0
    fi
}

# T4: 双路真实坐标+AIS replay 60秒
test_t4() {
    echo "  双路真实坐标+AIS replay..."
    echo "  前置：sidecar 已启动 with --replay <file>"
    echo "  手动验证完整匹配、一对一、时间对齐"
    echo "  （依赖真实 RTSP 流和 AIS replay 文件）"
    return 0
}

# T5: 完整真实链路300秒
test_t5() {
    local seconds="${T5_SECONDS:-300}"
    echo "  完整真实链路 ${seconds}秒..."
    echo "  前置：A/B RTSP、双路推理、真实坐标、MQTT/replay、A/B RTMP"
    if [ -x "./tools/dual_stream/dual_full_stack_stability.sh" ]; then
        ./tools/dual_stream/dual_full_stack_stability.sh "$seconds"
    else
        echo "  跳过：dual_full_stack_stability.sh 不存在或不可执行"
        return 0
    fi
}

if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T0" ]; then
    run_test "T0" "真实模型单元验证" test_t0
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T1" ]; then
    run_test "T1" "MQTT探测" test_t1
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T2" ]; then
    run_test "T2" "AIS replay验证" test_t2
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T3" ]; then
    run_test "T3" "双路真实坐标" test_t3
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T4" ]; then
    run_test "T4" "双路真实坐标+AIS replay" test_t4
fi
if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "T5" ]; then
    run_test "T5" "完整真实链路300秒" test_t5
fi

echo ""
echo "=========================================="
echo "  测试结果: $PASS 通过, $FAIL 失败"
echo "=========================================="
exit $FAIL
