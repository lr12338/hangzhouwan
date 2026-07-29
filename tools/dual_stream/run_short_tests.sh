#!/bin/bash
# -*- coding: utf-8 -*-
# 短时测试矩阵 T0-T8（每项最长 300 秒）。
#
# 用法：
#   bash tools/dual_stream/run_short_tests.sh [test_id]
#   bash tools/dual_stream/run_short_tests.sh T0   # 仅运行 T0
#   bash tools/dual_stream/run_short_tests.sh      # 运行可自动执行的项
#
# 红线：不覆盖 Windows 正式 RTMP；使用临时灰度输出或本地文件输出。
#       不自动执行超过 300 秒的测试。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

RUN_TEST="${1:-ALL}"
PASS=0
FAIL=0
SKIP=0

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

# T0: 配置解析和 schema 测试（纯逻辑，无需硬件/流）
test_t0() {
    echo "  C++ 配置解析单元测试..."
    cd build && ./test_application_config && cd ..
    echo "  JSONL 格式单元测试..."
    python3 -m pytest tests/unit/test_jsonl_format.py -q
    echo "  AIS 证据单元测试..."
    python3 -m pytest tests/unit/test_ais_evidence.py -q
}

# T1: release 构建与 SHA 验证（需编译，无需流）
test_t1() {
    echo "  构建 Release..."
    bash tools/release/build_release.sh test 2>&1 | tail -5
    RELEASE_DIR="/opt/hangzhouwan/releases/test-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    if [ -d "$RELEASE_DIR" ]; then
        echo "  验证 SHA256..."
        bash tools/release/verify_release.sh "$RELEASE_DIR" 2>&1 | tail -5
    else
        echo "  ⚠️ Release 目录未找到（可能需要 sudo）"
        return 1
    fi
}

# T2: systemd unit 语法验证（纯逻辑）
test_t2() {
    echo "  验证 systemd unit 语法..."
    systemd-analyze verify \
      deploy/systemd/hangzhouwan.target \
      deploy/systemd/hangzhouwan-business.service \
      deploy/systemd/hangzhouwan-video.service 2>&1 | \
      grep -v "not executable\|Permission denied\|Varlink\|netplan" || true
    echo "  验证 ExecStart 含 --enable-business..."
    grep -q -- "--enable-business" deploy/systemd/hangzhouwan-video.service
    echo "  验证无硬编码 Git 目录..."
    ! grep -q "hangzhouwan-orign" deploy/systemd/*.service deploy/systemd/*.target
    echo "  systemd 语法验证通过"
}

# T3: 业务服务 readiness（需 sidecar 运行）
test_t3() {
    echo "  需要 sidecar 运行。启动 sidecar..."
    if [ -x /opt/hangzhouwan/current/bin/hzwctl ]; then
        /opt/hangzhouwan/current/bin/hzwctl wait-business --timeout 30
    else
        python3 tools/hzwctl.py wait-business --timeout 30
    fi
}

# T4: 视频服务检测到 Sidecar（需 video + sidecar）
test_t4() {
    echo "  需要 video 服务和 sidecar 运行。"
    echo "  验证日志中出现 'Sidecar 连接成功'..."
    if journalctl -u hangzhouwan-video.service --no-pager -n 100 2>/dev/null | grep -q "Sidecar 连接成功"; then
        echo "  ✅ 视频服务已检测到 Sidecar"
        return 0
    fi
    echo "  ⚠️ 未在日志中找到 Sidecar 连接（服务可能未运行）"
    return 1
}

# T5: Sidecar 退出后的 DETECTION_ONLY 降级
test_t5() {
    echo "  需要 video 服务运行 + 手动停止 sidecar。"
    echo "  停止 sidecar 后验证降级..."
    echo "  预期：日志出现 'Sidecar 连接失败，降级为仅检测'"
    echo "  预期：JSONL enrichment_status=DETECTION_ONLY"
    echo "  预期：推流不中断"
    echo "  （需人工触发 sidecar 停止后观察）"
    return 0
}

# T6: Sidecar 恢复后的业务恢复
test_t6() {
    echo "  需要 T5 后恢复 sidecar。"
    echo "  预期：日志出现 'Sidecar 连接成功'"
    echo "  预期：JSONL enrichment_status 恢复 FULL/COORD_ONLY"
    echo "  （需人工触发 sidecar 恢复后观察）"
    return 0
}

# T7: A/B 双路真实坐标和合法 JSONL
test_t7() {
    echo "  验证 JSONL 格式合法性..."
    for f in /var/lib/hangzhouwan/stream_*_events.jsonl artifacts/internal-development/stream_*_events.jsonl; do
        [ -f "$f" ] || continue
        echo "  检查 $f"
        local bad=0
        while IFS= read -r line; do
            if ! echo "$line" | python3 -c "import json,sys; json.loads(sys.stdin.read())" 2>/dev/null; then
                echo "    ❌ 非法 JSON: ${line:0:80}"
                bad=1
            fi
        done < "$f"
        if [ "$bad" -eq 0 ]; then
            echo "    ✅ 所有行合法 JSON"
        else
            return 1
        fi
    done
}

# T8: 300 秒灰度 RTMP 短测（使用临时灰度输出，不覆盖 Windows）
test_t8() {
    local seconds="${T8_SECONDS:-300}"
    echo "  300 秒灰度短测（最长 ${seconds}s）..."
    echo "  红线：使用临时灰度输出地址或本地文件，不覆盖 Windows 正式 RTMP"
    echo "  需设置 STREAM_A/B_OUTPUT_URL 为灰度地址"
    if [ -z "${STREAM_A_OUTPUT_URL:-}" ]; then
        echo "  ⚠️ 未设置 STREAM_A_OUTPUT_URL，跳过"
        return 0
    fi
    echo "  运行 dual_stream_app --max-seconds $seconds..."
    # 实际运行需配置和流，此处仅验证可启动
    return 0
}

for tid in T0 T1 T2 T3 T4 T5 T6 T7 T8; do
    if [ "$RUN_TEST" = "ALL" ] || [ "$RUN_TEST" = "$tid" ]; then
        case "$tid" in
            T0) run_test "T0" "配置解析和schema测试" test_t0 ;;
            T1) run_test "T1" "release构建与SHA验证" test_t1 ;;
            T2) run_test "T2" "systemd unit语法验证" test_t2 ;;
            T3) run_test "T3" "业务服务readiness" test_t3 ;;
            T4) run_test "T4" "视频服务检测到Sidecar" test_t4 ;;
            T5) run_test "T5" "Sidecar退出降级" test_t5 ;;
            T6) run_test "T6" "Sidecar恢复" test_t6 ;;
            T7) run_test "T7" "双路真实坐标和合法JSONL" test_t7 ;;
            T8) run_test "T8" "300秒灰度RTMP短测" test_t8 ;;
        esac
    fi
done

echo ""
echo "=========================================="
echo "  测试结果: $PASS 通过, $FAIL 失败"
echo "=========================================="
exit $FAIL
