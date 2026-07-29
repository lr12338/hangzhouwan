# -*- coding: utf-8 -*-
"""日志和数据保留轮转。

支持：
  - 视频日志轮转
  - Sidecar 日志轮转
  - JSONL 事件轮转
  - AIS 原始消息轮转
  - 指标 CSV 轮转
  - 磁盘剩余空间保护

磁盘低于阈值时：
  1. 停止抓拍和原始 AIS 录制
  2. 保留核心视频推流
  3. 输出高等级告警
  4. 不因日志写满导致主程序崩溃
"""
import os
import time
import threading
import shutil


class LogRotator:
    """日志文件轮转器，按大小和保留数量管理。"""

    def __init__(self, max_size_mb=50, max_files=10):
        self.max_size_bytes = max_size_mb * 1024 * 1024
        self.max_files = max_files

    def rotate_if_needed(self, filepath):
        """如果文件超过最大大小，轮转。"""
        if not os.path.exists(filepath):
            return False
        if os.path.getsize(filepath) < self.max_size_bytes:
            return False
        # 轮转：file -> file.1 -> file.2 -> ... -> delete oldest
        for i in range(self.max_files - 1, 0, -1):
            old = f"{filepath}.{i}"
            new = f"{filepath}.{i + 1}"
            if os.path.exists(old):
                if i + 1 >= self.max_files:
                    os.remove(old)
                else:
                    os.rename(old, new)
        os.rename(filepath, f"{filepath}.1")
        return True

    def cleanup_dir(self, dirpath, prefix="", suffix=".log"):
        """清理目录中超出保留数量的旧文件。"""
        if not os.path.isdir(dirpath):
            return 0
        files = []
        for f in os.listdir(dirpath):
            if prefix and not f.startswith(prefix):
                continue
            if suffix and not f.endswith(suffix):
                continue
            files.append(os.path.join(dirpath, f))
        files.sort(key=os.path.getmtime, reverse=True)
        deleted = 0
        for f in files[self.max_files:]:
            try:
                os.remove(f)
                deleted += 1
            except Exception:
                pass
        return deleted


class DiskProtector:
    """磁盘空间保护器。

    磁盘低于阈值时停止抓拍和录制，保留核心推流。
    """

    def __init__(self, threshold_mb=500, check_interval_sec=30):
        self.threshold_mb = threshold_mb
        self.check_interval_sec = check_interval_sec
        self._stop = False
        self._thread = None
        self.protected = False
        self._lock = threading.Lock()
        self._callbacks = []

    def register_callback(self, callback):
        """注册磁盘保护回调（接收 bool: True=保护启动, False=恢复）。"""
        self._callbacks.append(callback)

    def get_disk_free_mb(self, path="."):
        try:
            usage = shutil.disk_usage(path)
            return usage.free // (1024 * 1024)
        except Exception:
            return 999999

    def check(self, path="."):
        """检查磁盘空间，返回是否需要保护。"""
        free_mb = self.get_disk_free_mb(path)
        with self._lock:
            was_protected = self.protected
            if free_mb < self.threshold_mb:
                self.protected = True
            else:
                self.protected = False
            changed = was_protected != self.protected
        if changed:
            for cb in self._callbacks:
                try:
                    cb(self.protected)
                except Exception:
                    pass
            if self.protected:
                print(f"警告 | 磁盘 | 剩余空间 {free_mb}MB < {self.threshold_mb}MB，"
                      f"启动磁盘保护（停止抓拍和录制）", flush=True)
            else:
                print(f"信息 | 磁盘 | 剩余空间 {free_mb}MB >= {self.threshold_mb}MB，"
                      f"恢复正常", flush=True)
        return self.protected

    def start_monitor(self, path="."):
        def _loop():
            while not self._stop:
                self.check(path)
                time.sleep(self.check_interval_sec)
        self._thread = threading.Thread(target=_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
