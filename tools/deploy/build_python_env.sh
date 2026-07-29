#!/bin/bash
# -*- coding: utf-8 -*-
# 在 x86 服务器上构建 ARM64 离线 wheelhouse。
#
# 用法：
#   bash tools/deploy/build_python_env.sh [output_dir]
#
# 前置条件：
#   - Docker（支持 multiarch/qemu）
#   - 或直接在 ARM64 机器上运行

set -euo pipefail

OUTPUT_DIR="${1:-artifacts/python-wheelhouse-arm64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REQ_FILE="$REPO_ROOT/services/business_enrichment/requirements.lock"

echo "=== 构建 ARM64 离线 wheelhouse ==="
echo "输出目录: $OUTPUT_DIR"
echo "依赖文件: $REQ_FILE"

mkdir -p "$OUTPUT_DIR"

# 下载 wheel 包（在 ARM64 机器上直接 pip download）
pip3 download \
    -r "$REQ_FILE" \
    -d "$OUTPUT_DIR" \
    --platform manylinux2014_aarch64 \
    --python-version 38 \
    --only-binary=:all: \
    --no-deps \
    2>/dev/null || true

# 也下载 pure-python 包
pip3 download \
    -r "$REQ_FILE" \
    -d "$OUTPUT_DIR" \
    --no-deps \
    --no-binary=:all: \
    2>/dev/null || true

# 生成 manifest
echo "=== 生成 wheelhouse manifest ==="
python3 -c "
import hashlib, json, os, sys
wheelhouse = sys.argv[1]
files = []
for f in sorted(os.listdir(wheelhouse)):
    if f.endswith(('.whl', '.tar.gz')):
        path = os.path.join(wheelhouse, f)
        with open(path, 'rb') as fh:
            sha = hashlib.sha256(fh.read()).hexdigest()
        files.append({'file': f, 'sha256': sha, 'size': os.path.getsize(path)})
manifest = {'generated_at': __import__('time').strftime('%Y-%m-%dT%H:%M:%S%z'), 'files': files}
with open(os.path.join(wheelhouse, 'wheelhouse-manifest.json'), 'w') as fh:
    json.dump(manifest, fh, indent=2)
with open(os.path.join(wheelhouse, 'sha256sum.txt'), 'w') as fh:
    for f in files:
        fh.write(f'{f[\"sha256\"]}  {f[\"file\"]}\n')
print(f'  {len(files)} packages')
" "$OUTPUT_DIR"

echo "=== wheelhouse 构建完成 ==="
echo "安装方式："
echo "  python3 -m venv /data/hangzhouwan/venv"
echo "  pip install --no-index --find-links $OUTPUT_DIR -r $REQ_FILE"
