#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""业务 Sidecar 兼容启动入口。

此文件仅负责调用正式服务包 services.business_enrichment.app，
不再承载全部实现。保留以确保旧启动脚本和文档兼容。
"""
import os
import sys

# 确保 repo root 在 path 中
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from services.business_enrichment.app import main

if __name__ == "__main__":
    main()
