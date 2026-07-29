#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把板端固定版本 Python distribution 复制到隔离 venv。

不生成指向用户 site-packages 的 .pth；Release 激活后可设置
PYTHONNOUSERSITE=1，并在 ProtectHome=true 下独立运行。
"""

import importlib.metadata
import importlib.util
import os
import shutil
import sys

REQUIRED = (
    "numpy",
    "scipy",
    "scikit-learn",
    "joblib",
    "threadpoolctl",
    "paho-mqtt",
    "pyais",
    "bitarray",
    "attrs",
    "PyYAML",
)

FALLBACK_IMPORTS = {
    "attrs": ("attr", "attrs"),
    "PyYAML": ("yaml",),
}


def copy_fallback_imports(name, distribution, destination):
    copied = 0
    for import_name in FALLBACK_IMPORTS.get(name, ()):
        spec = importlib.util.find_spec(import_name)
        if not spec:
            continue
        if spec.submodule_search_locations:
            source = os.path.realpath(next(iter(spec.submodule_search_locations)))
            target = os.path.join(destination, os.path.basename(source))
            shutil.copytree(
                source, target, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
            )
            copied += sum(len(files) for _, _, files in os.walk(target))
        elif spec.origin:
            source = os.path.realpath(spec.origin)
            target = os.path.join(destination, os.path.basename(source))
            shutil.copy2(source, target)
            copied += 1

    # Debian 的 egg-info 可能没有 RECORD/SOURCES，仍复制版本元数据。
    metadata_path = os.path.realpath(str(distribution._path))
    metadata_target = os.path.join(destination, os.path.basename(metadata_path))
    if os.path.isdir(metadata_path):
        shutil.copytree(metadata_path, metadata_target, dirs_exist_ok=True)
        copied += sum(
            len(files) for _, _, files in os.walk(metadata_target))
    elif os.path.isfile(metadata_path):
        shutil.copy2(metadata_path, metadata_target)
        copied += 1
    return copied


def copy_distribution(name, destination):
    distribution = importlib.metadata.distribution(name)
    source_root = os.path.realpath(str(distribution.locate_file("")))
    copied = 0
    for entry in distribution.files or ():
        source = os.path.realpath(str(distribution.locate_file(entry)))
        if not (source == source_root or source.startswith(source_root + os.sep)):
            continue
        relative = os.path.relpath(source, source_root)
        target = os.path.join(destination, relative)
        if os.path.isdir(source):
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(source, target)
        copied += 1
    if not copied:
        copied = copy_fallback_imports(name, distribution, destination)
    if not copied:
        raise RuntimeError(f"{name} 没有可复制文件")
    print(f"  {name}=={distribution.version}: {copied} files")


def main():
    if len(sys.argv) != 2:
        print("usage: vendor_python_runtime.py DEST_SITE_PACKAGES",
              file=sys.stderr)
        return 2
    destination = os.path.abspath(sys.argv[1])
    os.makedirs(destination, exist_ok=True)
    for name in REQUIRED:
        copy_distribution(name, destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
