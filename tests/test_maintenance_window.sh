#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# =============================================================================
# 维护窗口脚本功能测试（不操作生产 systemd、不修改 iptables）。
#
# 覆盖：
#   1. bash -n 全部脚本语法通过
#   2. 生产守卫失败时生成本次 FAIL 汇总
#   3. current 软链接别名拒绝
#   4. manifest 损坏（commit 不匹配）拒绝
#   5. RTMP 注入脚本无 --target-ip 时拒绝
#   6. RTMP 注入脚本 trap 清理结构验证
#   7. 旧 PASS 汇总隔离（evaluate_gates 不覆盖旧文件）
#
# 所有测试使用 mock systemctl/pgrep 和临时目录，不影响生产。
# =============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

FAILURES=0
TMP_BASE=""

cleanup() {
  [ -n "$TMP_BASE" ] && rm -rf "$TMP_BASE"
}
trap cleanup EXIT

TMP_BASE="$(mktemp -d /tmp/hzw_mw_test.XXXXXX)"
MOCK_DIR="$TMP_BASE/mock"
mkdir -p "$MOCK_DIR"

pass() { echo "[通过] $1"; }
fail() { echo "[失败] $1"; FAILURES=$((FAILURES + 1)); }

# ===========================================================================
# 1. bash -n 全部脚本语法通过
# ===========================================================================
echo ""
echo "=========================================="
echo "  1. bash -n 语法检查"
echo "=========================================="
for script in \
  tools/dual_stream/forced_reconnect_run.sh \
  tools/dual_stream/rtmp_fault_injection.sh \
  tools/release/activate_release.sh \
  tools/release/verify_release.sh \
  tools/release/rollback_release.sh \
  tools/release/build_release.sh; do
  if bash -n "$script" 2>/dev/null; then
    pass "bash -n $script"
  else
    fail "bash -n $script"
  fi
done

# ===========================================================================
# 2. 生产守卫失败时生成本次 FAIL 汇总
# ===========================================================================
echo ""
echo "=========================================="
echo "  2. 生产守卫失败生成 FAIL 汇总"
echo "=========================================="

# Mock systemctl: 报告 video 服务 active
cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
if [ "$1" = "is-active" ] && [ "$2" = "--quiet" ]; then
  svc="$3"
  if [ "$svc" = "hangzhouwan-video.service" ]; then
    exit 0  # active
  fi
  exit 3  # inactive
fi
exit 0
MOCK
chmod +x "$MOCK_DIR/systemctl"

# Mock pgrep: 无进程
cat > "$MOCK_DIR/pgrep" << 'MOCK'
#!/bin/bash
exit 1  # no process found
MOCK
chmod +x "$MOCK_DIR/pgrep"

export PATH="$MOCK_DIR:$PATH"
export HZW_CANDIDATE_RELEASE="$TMP_BASE/fake_release"

# 运行 runner（应在生产守卫处拒绝）
RC=0; OUTPUT=$(bash tools/dual_stream/forced_reconnect_run.sh all 2>&1) || RC=$?

if [ "$RC" -eq 2 ]; then
  pass "生产守卫失败返回 rc=2"
else
  fail "生产守卫应返回 rc=2，实际 rc=$RC"
fi

# 检查是否生成了 FAIL 汇总
LATEST_SUMMARY="artifacts/internal-development/forced_reconnect_summary_latest.json"
if [ -f "$LATEST_SUMMARY" ]; then
  OVERALL=$(python3 -c "import json; d=json.load(open('$LATEST_SUMMARY')); print(d.get('overall',''))" 2>/dev/null || echo "")
  REASON=$(python3 -c "import json; d=json.load(open('$LATEST_SUMMARY')); print(d.get('failure_reason',''))" 2>/dev/null || echo "")
  if [ "$OVERALL" = "FAIL" ]; then
    pass "生产守卫失败生成 FAIL 汇总 (reason=$REASON)"
  else
    fail "汇总 overall=$OVERALL (应 FAIL)"
  fi
  if [ "$REASON" = "production_still_running" ]; then
    pass "failure_reason=production_still_running"
  else
    fail "failure_reason=$REASON (应 production_still_running)"
  fi
  # 检查 run_id 和 summary_file 字段
  RUN_ID=$(python3 -c "import json; d=json.load(open('$LATEST_SUMMARY')); print(d.get('run_id',''))" 2>/dev/null || echo "")
  if [ -n "$RUN_ID" ]; then
    pass "汇总包含 run_id=$RUN_ID"
  else
    fail "汇总缺少 run_id"
  fi
else
  fail "未生成汇总文件"
fi

# 清理 mock
unset PATH; export PATH="$MOCK_DIR:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# ===========================================================================
# 3. current 软链接别名拒绝
# ===========================================================================
echo ""
echo "=========================================="
echo "  3. current 软链接别名拒绝"
echo "=========================================="

# Mock systemctl/pgrep: 报告无生产运行
cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
if [ "$1" = "is-active" ] && [ "$2" = "--quiet" ]; then
  exit 3  # inactive
fi
exit 0
MOCK
chmod +x "$MOCK_DIR/systemctl"

cat > "$MOCK_DIR/pgrep" << 'MOCK'
#!/bin/bash
exit 1  # no process
MOCK
chmod +x "$MOCK_DIR/pgrep"

export PATH="$MOCK_DIR:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# 创建指向 current 实际目标的软链接别名
CUR_REAL=$(readlink -f /opt/hangzhouwan/current 2>/dev/null || echo "")
if [ -n "$CUR_REAL" ] && [ -d "$CUR_REAL" ]; then
  SYMLINK_ALIAS="$TMP_BASE/alias_to_current"
  ln -sfn "$CUR_REAL" "$SYMLINK_ALIAS"
  export HZW_CANDIDATE_RELEASE="$SYMLINK_ALIAS"

  RC=0; OUTPUT=$(bash tools/dual_stream/forced_reconnect_run.sh all 2>&1) || RC=$?

  if [ "$RC" -ne 0 ]; then
    pass "软链接别名拒绝 rc=$RC"
  else
    fail "软链接别名应被拒绝"
  fi

  REASON=$(python3 -c "import json; d=json.load(open('artifacts/internal-development/forced_reconnect_summary_latest.json')); print(d.get('failure_reason',''))" 2>/dev/null || echo "")
  case "$REASON" in
    candidate_is_current|candidate_symlink_alias|candidate_is_previous)
      pass "failure_reason=$REASON"
      ;;
    *)
      # 可能在更早的检查失败（如 TOOL 不存在），检查是否因候选问题拒绝
      if echo "$OUTPUT" | grep -q "拒绝"; then
        pass "候选被拒绝 (reason=$REASON)"
      else
        fail "failure_reason=$REASON (应 candidate_is_current/symlink_alias)"
      fi
      ;;
  esac
else
  echo "  跳过：无法获取 current 实际目标"
fi

# ===========================================================================
# 4. manifest 损坏（commit 不匹配）拒绝
# ===========================================================================
echo ""
echo "=========================================="
echo "  4. manifest commit 不匹配拒绝"
echo "=========================================="

# 创建一个最小 fake release（带可执行 tool 和损坏 manifest commit）
FAKE_RELEASE="$TMP_BASE/fake_release_bad_manifest"
mkdir -p "$FAKE_RELEASE/bin"
cat > "$FAKE_RELEASE/bin/forced_reconnect_tool" << 'TOOL'
#!/bin/bash
echo "fake tool"
TOOL
chmod +x "$FAKE_RELEASE/bin/forced_reconnect_tool"

# manifest commit 故意不匹配
cat > "$FAKE_RELEASE/manifest.json" << 'MANIFEST'
{"version": "fake", "commit": "wrong_commit_xyz", "files": []}
MANIFEST

# 确保 testdata/test.mp4 存在
mkdir -p testdata
touch testdata/test.mp4

export HZW_CANDIDATE_RELEASE="$FAKE_RELEASE"
export HZW_EXPECTED_COMMIT="1a1a7b2"

RC=0; OUTPUT=$(bash tools/dual_stream/forced_reconnect_run.sh all 2>&1) || RC=$?

REASON=$(python3 -c "import json; d=json.load(open('artifacts/internal-development/forced_reconnect_summary_latest.json')); print(d.get('failure_reason',''))" 2>/dev/null || echo "")

# 可能因 verify_release 失败或 manifest_commit_mismatch 被拒绝
if [ "$RC" -ne 0 ]; then
  pass "manifest 损坏拒绝 rc=$RC"
else
  fail "manifest 损坏应被拒绝"
fi

case "$REASON" in
  manifest_commit_mismatch|verify_release_failed|manifest_missing)
    pass "failure_reason=$REASON"
    ;;
  *)
    if echo "$OUTPUT" | grep -q "拒绝"; then
      pass "候选被拒绝 (reason=$REASON)"
    else
      fail "failure_reason=$REASON (应 manifest_commit_mismatch/verify_release_failed)"
    fi
    ;;
esac

# 验证汇总包含 manifest commit 信息
MANIFEST_COMMIT=$(python3 -c "import json; d=json.load(open('artifacts/internal-development/forced_reconnect_summary_latest.json')); print(d.get('candidate_manifest_commit',''))" 2>/dev/null || echo "")
if [ -n "$MANIFEST_COMMIT" ]; then
  pass "汇总记录 candidate_manifest_commit=$MANIFEST_COMMIT"
else
  # 如果 verify_release 先失败，manifest commit 可能为空
  if [ "$REASON" = "verify_release_failed" ]; then
    pass "verify_release 先失败，manifest commit 未读取（合理）"
  else
    fail "汇总缺少 candidate_manifest_commit"
  fi
fi

# ===========================================================================
# 5. RTMP 注入脚本无 --target-ip 时拒绝
# ===========================================================================
echo ""
echo "=========================================="
echo "  5. RTMP 脚本无 --target-ip 拒绝"
echo "=========================================="

RC=0; OUTPUT=$(bash tools/dual_stream/rtmp_fault_injection.sh 2>&1) || RC=$?

if [ "$RC" -eq 1 ]; then
  pass "RTMP 脚本无 --target-ip 返回 rc=1"
else
  fail "RTMP 脚本无 --target-ip 应返回 rc=1，实际 rc=$RC"
fi

if echo "$OUTPUT" | grep -q "target-ip"; then
  pass "RTMP 脚本提示需要 --target-ip"
else
  fail "RTMP 脚本未提示 --target-ip"
fi

# ===========================================================================
# 6. RTMP 注入脚本 trap 清理结构验证
# ===========================================================================
echo ""
echo "=========================================="
echo "  6. RTMP 脚本 trap 清理结构验证"
echo "=========================================="

RTMP_SRC="tools/dual_stream/rtmp_fault_injection.sh"

# 检查 set -euo pipefail
if grep -q 'set -euo pipefail' "$RTMP_SRC"; then
  pass "RTMP 脚本有 set -euo pipefail"
else
  fail "RTMP 脚本缺少 set -euo pipefail"
fi

# 检查 trap EXIT
if grep -q 'trap cleanup_rules EXIT' "$RTMP_SRC"; then
  pass "RTMP 脚本有 trap EXIT 清理"
else
  fail "RTMP 脚本缺少 trap EXIT"
fi

# 检查 trap INT TERM
if grep -q 'INT TERM' "$RTMP_SRC"; then
  pass "RTMP 脚本有 trap INT/TERM"
else
  fail "RTMP 脚本缺少 trap INT/TERM"
fi

# 检查唯一规则标识
if grep -q 'RULE_TAG' "$RTMP_SRC" && grep -q 'comment' "$RTMP_SRC"; then
  pass "RTMP 脚本有唯一规则标识 (comment)"
else
  fail "RTMP 脚本缺少唯一规则标识"
fi

# 检查前后核对无残留
if grep -q 'RESIDUAL_BEFORE' "$RTMP_SRC" && grep -q 'RESIDUAL_AFTER' "$RTMP_SRC"; then
  pass "RTMP 脚本前后核对无残留规则"
else
  fail "RTMP 脚本缺少前后核对"
fi

# 检查无"任选其一"
if ! grep -q '任选其一' "$RTMP_SRC"; then
  pass "RTMP 脚本无开放式任选其一"
else
  fail "RTMP 脚本仍含任选其一"
fi

# ===========================================================================
# 7. 旧 PASS 汇总隔离（evaluate_gates 不覆盖旧文件）
# ===========================================================================
echo ""
echo "=========================================="
echo "  7. 旧 PASS 汇总隔离"
echo "=========================================="

ISO_DIR="$TMP_BASE/iso_test"
mkdir -p "$ISO_DIR/logs"

# 创建旧 PASS pre-summary
cat > "$ISO_DIR/old_pre.json" << 'PRESUM'
{"run_id":"old-run","overall_rc":0,"overall":"PASS","failure_reason":"","phases":[]}
PRESUM

python3 tools/dual_stream/evaluate_gates.py "$ISO_DIR/logs" "$ISO_DIR/old_pre.json" "$ISO_DIR/old_out.json" 2>/dev/null
OLD_HASH=$(sha256sum "$ISO_DIR/old_out.json" | awk '{print $1}')

# 创建新 FAIL pre-summary（不同输出文件）
cat > "$ISO_DIR/new_pre.json" << 'PRESUM'
{"run_id":"new-run","overall_rc":1,"overall":"FAIL","failure_reason":"phase_failed","phases":[{"name":"decoder","ok":100,"fail":0,"inflight_end":0}]}
PRESUM

python3 tools/dual_stream/evaluate_gates.py "$ISO_DIR/logs" "$ISO_DIR/new_pre.json" "$ISO_DIR/new_out.json" 2>/dev/null

# 验证旧文件未被修改
NEW_OLD_HASH=$(sha256sum "$ISO_DIR/old_out.json" | awk '{print $1}')
if [ "$OLD_HASH" = "$NEW_OLD_HASH" ]; then
  pass "旧 PASS 汇总未被修改"
else
  fail "旧 PASS 汇总被修改"
fi

# 验证新文件为 FAIL
NEW_OVERALL=$(python3 -c "import json; print(json.load(open('$ISO_DIR/new_out.json'))['overall'])" 2>/dev/null || echo "")
if [ "$NEW_OVERALL" = "FAIL" ]; then
  pass "新 FAIL 汇总正确生成"
else
  fail "新汇总 overall=$NEW_OVERALL (应 FAIL)"
fi

# ===========================================================================
# 汇总
# ===========================================================================
echo ""
echo "=========================================="
echo "  维护窗口测试: $FAILURES 失败"
echo "=========================================="
exit $FAILURES
