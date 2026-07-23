#!/bin/bash
# -*- coding: utf-8 -*-
# 构建不可变 Release 制品。
#
# 用法：
#   bash tools/release/build_release.sh [version]
#
# 生成 /opt/hangzhouwan/releases/<version>-<commit>/ 包含：
#   bin/dual_stream_app, bin/hzwctl, venv/, models/, systemd/, config/,
#   services/, VERSION, manifest.json, sha256sum.txt
#
# 不自动 activate。不依赖 Git 工作区运行生产服务。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RELEASE_ROOT="/opt/hangzhouwan/releases"
GIT_COMMIT="$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
VERSION="${1:-$(date +%Y%m%d%H%M)}"
RELEASE_NAME="${VERSION}-${GIT_COMMIT}"
RELEASE_DIR="${RELEASE_ROOT}/${RELEASE_NAME}"

echo "=== 构建 Release: ${RELEASE_NAME} ==="

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
# hzwctl（优先使用已构建版本，回退到 Python 脚本）
if [ -f "$REPO_ROOT/build/hzwctl" ]; then
  cp "$REPO_ROOT/build/hzwctl" "$RELEASE_DIR/bin/"
elif [ -f "$REPO_ROOT/tools/hzwctl.py" ]; then
  cp "$REPO_ROOT/tools/hzwctl.py" "$RELEASE_DIR/bin/hzwctl"
fi
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
cp "$REPO_ROOT"/deploy/systemd/*.target "$RELEASE_DIR/systemd/" 2>/dev/null || true
cp "$REPO_ROOT/config/application.example.yaml" "$RELEASE_DIR/config/"
# tmpfiles.d：共享运行目录 /run/hangzhouwan 由 tmpfiles.d 统一管理
if [ -f "$REPO_ROOT/deploy/tmpfiles.d/hangzhouwan.conf" ]; then
  mkdir -p "$RELEASE_DIR/tmpfiles.d"
  cp "$REPO_ROOT/deploy/tmpfiles.d/hangzhouwan.conf" "$RELEASE_DIR/tmpfiles.d/"
fi
cp -r "$REPO_ROOT/services" "$RELEASE_DIR/"
find "$RELEASE_DIR/services" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

# 6. 准备 Python venv（离线 wheelhouse 或符号链接到系统 venv）
echo "[6/7] 准备 Python 环境..."
if [ -d "/data/hangzhouwan/venv" ]; then
  # 复用已有 venv
  echo "  复用 /data/hangzhouwan/venv"
  ln -sf /data/hangzhouwan/venv "$RELEASE_DIR/venv"
else
  # 本板 ensurepip 不可用且 sklearn/scipy/joblib/pyais/paho 位于用户 site，
  # 使用 --system-site-packages --without-pip 创建 venv，并通过 .pth 继承用户 site。
  python3 -m venv --system-site-packages --without-pip "$RELEASE_DIR/venv" 2>/dev/null || true
  if [ -x "$RELEASE_DIR/venv/bin/python3" ]; then
    PYVER="$("$RELEASE_DIR/venv/bin/python3" -c 'import sys;print("%d.%d"%sys.version_info[:2])' 2>/dev/null)"
    SP_DIR="$RELEASE_DIR/venv/lib/python${PYVER}/site-packages"
    USER_LOCAL="$HOME/.local/lib/python${PYVER}/site-packages"
    mkdir -p "$SP_DIR" 2>/dev/null || true
    if [ -n "$USER_LOCAL" ] && [ -d "$USER_LOCAL" ]; then
      echo "$USER_LOCAL" > "$SP_DIR/userlocal.pth"
    fi
    "$RELEASE_DIR/venv/bin/python3" -c "import sklearn,scipy,joblib,numpy,pyais,paho.mqtt.client" 2>/dev/null \
      && echo "  venv 依赖 OK (继承系统+用户 site)" \
      || echo "  警告: venv 依赖不完整，请手动安装 sklearn/scipy/joblib/pyais/paho"
  fi
fi

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

# sha256sum.txt（排除 venv 符号链接和 manifest/sha 自身）
cd "$RELEASE_DIR"
find . -type f ! -name 'sha256sum.txt' ! -name 'manifest.json' \
  ! -path './venv/*' -print0 | sort -z | xargs -0 sha256sum > sha256sum.txt
echo "  sha256sum.txt: $(wc -l < sha256sum.txt) entries"

echo ""
echo "=== Release 构建完成: ${RELEASE_DIR} ==="
echo "验证: bash tools/release/verify_release.sh ${RELEASE_DIR}"
echo "激活: bash tools/release/activate_release.sh ${RELEASE_DIR}"
