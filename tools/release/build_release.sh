#!/bin/bash
# -*- coding: utf-8 -*-
# 构建不可变 Release 制品。
#
# 用法：
#   bash tools/release/build_release.sh [version]
#
# 生成 /data/hangzhouwan/releases/<version>-<commit>/ 包含：
#   bin/dual_stream_app, bin/hzwctl, venv/, models/, systemd/, config/,
#   services/, VERSION, manifest.json, sha256sum.txt
#
# 不自动 activate。不依赖 Git 工作区运行生产服务。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RELEASE_ROOT="/data/hangzhouwan/releases"
GIT_COMMIT="$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
VERSION="${1:-$(date +%Y%m%d%H%M)}"
RELEASE_NAME="${VERSION}-${GIT_COMMIT}"
RELEASE_DIR="${RELEASE_ROOT}/${RELEASE_NAME}"

echo "=== 构建 Release: ${RELEASE_NAME} ==="
if [ -e "$RELEASE_DIR" ]; then
  echo "错误: 不可变 Release 已存在，拒绝覆盖: $RELEASE_DIR" >&2
  exit 1
fi

# 1. 编译 C++ 二进制
echo "[1/7] 编译 C++ 二进制..."
cd "$REPO_ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
cmake --build build -j4 2>&1 | tail -3

# 2. 创建 Release 目录
echo "[2/7] 创建 Release 目录..."
sudo mkdir -p "$RELEASE_DIR"/{bin,models,systemd,config,services}
sudo chown -R "$(id -u):$(id -g)" "$RELEASE_DIR"

# 3. 复制二进制
echo "[3/7] 复制二进制..."
cp "$REPO_ROOT/build/dual_stream_app" "$RELEASE_DIR/bin/"
# 板端 forced-reconnect/VPU 复现工具（维护窗口用）
if [ -f "$REPO_ROOT/build/forced_reconnect_tool" ]; then
  cp "$REPO_ROOT/build/forced_reconnect_tool" "$RELEASE_DIR/bin/"
fi
# hzwctl 固定从 Release 自带 venv 启动，禁止使用 system/user-site Python。
cp "$REPO_ROOT/tools/hzwctl.py" "$RELEASE_DIR/bin/hzwctl.py"
cp "$REPO_ROOT/tools/release/hzwctl-wrapper.sh" "$RELEASE_DIR/bin/hzwctl"
chmod 0755 "$RELEASE_DIR/bin/"*

# 4. 复制模型（外部权重资产，缺失即 fail-fast，不产出残缺 Release）
echo "[4/7] 复制模型..."
BMODEL="$REPO_ROOT/artifacts/bm1684-f32/yolov7_ship_1684_f32.bmodel"
PKL_A="$REPO_ROOT/weights/0121_random_forest_model.pkl"
PKL_B="$REPO_ROOT/weights/beishang_x-l.pkl"
# Release 构建依赖 Git 源码 + 固定 SHA256 的外部权重资产，缺一不可。
for asset in "$BMODEL" "$PKL_A" "$PKL_B"; do
  if [ ! -f "$asset" ]; then
    echo "  ❌ 必需外部资产缺失: $asset" >&2
    echo "  Release 构建依赖 Git 源码和固定 SHA256 的外部权重资产，缺失即中止。" >&2
    exit 1
  fi
done
cp "$BMODEL" "$RELEASE_DIR/models/"
cp "$PKL_A" "$RELEASE_DIR/models/"
cp "$PKL_B" "$RELEASE_DIR/models/"

# 5. 复制 systemd / config / services
echo "[5/7] 复制 systemd / config / services / tmpfiles.d..."
cp "$REPO_ROOT"/deploy/systemd/*.service "$RELEASE_DIR/systemd/" 2>/dev/null || true
cp "$REPO_ROOT"/deploy/systemd/*.timer "$RELEASE_DIR/systemd/" 2>/dev/null || true
cp "$REPO_ROOT"/deploy/systemd/*.target "$RELEASE_DIR/systemd/" 2>/dev/null || true
cp "$REPO_ROOT/config/application.example.yaml" "$RELEASE_DIR/config/"
# tmpfiles.d：共享运行目录 /run/hangzhouwan 由 tmpfiles.d 统一管理
if [ -f "$REPO_ROOT/deploy/tmpfiles.d/hangzhouwan.conf" ]; then
  mkdir -p "$RELEASE_DIR/tmpfiles.d"
  cp "$REPO_ROOT/deploy/tmpfiles.d/hangzhouwan.conf" "$RELEASE_DIR/tmpfiles.d/"
fi
for deploy_part in udev journald logrotate rsyslog; do
  if [ -d "$REPO_ROOT/deploy/$deploy_part" ]; then
    mkdir -p "$RELEASE_DIR/$deploy_part"
    cp -a "$REPO_ROOT/deploy/$deploy_part/." "$RELEASE_DIR/$deploy_part/"
  fi
done
cp -r "$REPO_ROOT/services" "$RELEASE_DIR/"
find "$RELEASE_DIR/services" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

# 6. 准备固定版本、自包含的 Python venv。构建时可从板端已固定的
# distribution 取文件，产物不含任何 /home 或 user-site .pth。
echo "[6/7] 准备自包含 Python 环境..."
python3 -m venv --without-pip --copies "$RELEASE_DIR/venv"
PYVER="$("$RELEASE_DIR/venv/bin/python3" -c 'import sys;print("%d.%d"%sys.version_info[:2])')"
SP_DIR="$RELEASE_DIR/venv/lib/python${PYVER}/site-packages"
python3 "$REPO_ROOT/tools/release/vendor_python_runtime.py" "$SP_DIR"
if find "$RELEASE_DIR/venv" -type f -name '*.pth' -exec grep -l '/home/' {} + | grep -q .; then
  echo "错误: venv 含 /home 依赖" >&2
  exit 1
fi
(
  cd "$RELEASE_DIR"
  PYTHONNOUSERSITE=1 "$RELEASE_DIR/venv/bin/python3" -c \
    "import yaml,numpy,scipy,sklearn,joblib,pyais,paho.mqtt.client,services.business_enrichment.app"
)
echo "  自包含 venv 依赖验证通过"

# 7. 生成 VERSION / manifest.json / sha256sum.txt
echo "[7/7] 生成元数据..."
cat > "$RELEASE_DIR/VERSION" <<VEREOF
${RELEASE_NAME}
commit: ${GIT_COMMIT}
build_date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
VEREOF

# manifest.json
BMODEL_SHA=""
if [ -f "$RELEASE_DIR/models/yolov7_ship_1684_f32.bmodel" ]; then
  BMODEL_SHA=$(sha256sum "$RELEASE_DIR/models/yolov7_ship_1684_f32.bmodel" | awk '{print $1}')
fi
COORD_A_SHA=""
if [ -f "$RELEASE_DIR/models/0121_random_forest_model.pkl" ]; then
  COORD_A_SHA=$(sha256sum "$RELEASE_DIR/models/0121_random_forest_model.pkl" | awk '{print $1}')
fi
COORD_B_SHA=""
if [ -f "$RELEASE_DIR/models/beishang_x-l.pkl" ]; then
  COORD_B_SHA=$(sha256sum "$RELEASE_DIR/models/beishang_x-l.pkl" | awk '{print $1}')
fi
python3 -c "
import json, os
d = os.path.join('$RELEASE_DIR')
files = []
for root, _, fs in os.walk(d):
    for f in fs:
        p = os.path.join(root, f)
        if os.path.islink(p):
            continue
        rel = os.path.relpath(p, d)
        files.append(rel)
files.extend(['manifest.json', 'sha256sum.txt'])
files.sort()
manifest = {
    'version': '${RELEASE_NAME}',
    'commit': '${GIT_COMMIT}',
    'bmodel_sha256': '${BMODEL_SHA}',
    'coord_model_a_sha256': '${COORD_A_SHA}',
    'coord_model_b_sha256': '${COORD_B_SHA}',
    'files': files,
}
with open(os.path.join(d, 'manifest.json'), 'w') as fh:
    json.dump(manifest, fh, indent=2, ensure_ascii=False)
print('  manifest.json: %d files' % len(files))
"

# sha256sum.txt（覆盖 manifest 和全部普通文件，仅排除校验表自身）
cd "$RELEASE_DIR"
find . -type f ! -name 'sha256sum.txt' \
  -print0 | sort -z | xargs -0 sha256sum > sha256sum.txt
echo "  sha256sum.txt: $(wc -l < sha256sum.txt) entries"

# Release 不可变：root 拥有，服务账户只读。
find "$RELEASE_DIR" -type d -exec chmod 0755 {} +
find "$RELEASE_DIR" -type f -exec chmod 0644 {} +
find "$RELEASE_DIR/bin" "$RELEASE_DIR/venv/bin" -type f -exec chmod 0755 {} +
sudo chown -R root:root "$RELEASE_DIR"

echo ""
echo "=== Release 构建完成: ${RELEASE_DIR} ==="
echo "验证: bash tools/release/verify_release.sh ${RELEASE_DIR}"
echo "激活: bash tools/release/activate_release.sh ${RELEASE_DIR}"
