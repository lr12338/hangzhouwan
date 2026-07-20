#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线测试入口（中文日志）。使用标准库 unittest，无需 pytest。
用法：python3 tests/run_tests.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)


def main():
    loader = unittest.TestLoader()
    suite = loader.discover(os.path.join(HERE, "unit"), pattern="test_*.py", top_level_dir=REPO)
    suite.addTests(loader.discover(os.path.join(HERE, "integration"), pattern="test_*.py", top_level_dir=REPO))
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    print("\n信息 | 测试框架 | " +
          (f"全部通过，共 {result.testsRun} 项" if result.wasSuccessful()
           else f"未通过：失败 {len(result.failures)}，错误 {len(result.errors)}"))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
