#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Python 环境自检：验证所有依赖版本正确。

用法：
  python3 tools/deploy/check_python_env.py
"""
import sys
import json

REQUIRED = {
    "numpy": "1.23.2",
    "scipy": "1.10.1",
    "sklearn": "1.3.2",
    "joblib": "1.4.2",
    "paho.mqtt.client": None,  # any version
    "pyais": "2.4.0",
    "bitarray": None,
}

OPTIONAL = {
    "attrs": None,
}


def check():
    results = {}
    all_ok = True
    for module, required_version in REQUIRED.items():
        try:
            mod = __import__(module, fromlist=[""])
            version = getattr(mod, "__version__", "unknown")
            if required_version and version != required_version:
                results[module] = {
                    "status": "MISMATCH",
                    "installed": version,
                    "required": required_version,
                }
                all_ok = False
            else:
                results[module] = {"status": "OK", "version": version}
        except ImportError:
            results[module] = {"status": "MISSING", "required": required_version or "any"}
            all_ok = False

    for module, required_version in OPTIONAL.items():
        try:
            mod = __import__(module, fromlist=[""])
            version = getattr(mod, "__version__", "unknown")
            results[module] = {"status": "OK", "version": version}
        except ImportError:
            results[module] = {"status": "MISSING (optional)"}

    print(json.dumps(results, indent=2))
    python_ver = "{}.{}.{}".format(*sys.version_info[:3])
    print(f"\nPython: {python_ver}")
    if all_ok:
        print("✅ Python 环境自检通过")
    else:
        print("❌ Python 环境自检未通过")
    return all_ok


if __name__ == "__main__":
    sys.exit(0 if check() else 1)
