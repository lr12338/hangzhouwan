#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""每日 03:30 受控维护：锁、前检、顺序重启与三次健康确认。"""

import fcntl
import os
import shutil
import socket
import subprocess
import sys
import time

from .supervisor import (
    AlertLog,
    MqttAlertPublisher,
    load_config,
    upstream_reachable,
)
from .maintenance_recovery import (
    arm_recovery_timer,
    collect_diagnostics,
    reset_failed,
    schedule_recovery,
    stop_services,
    current_availability,
)
from .availability import HEALTHY, OPERATIONAL_DEGRADED

LOCK_PATH = "/run/hangzhouwan/maintenance.lock"
HZWCTL = "/opt/hangzhouwan/current/bin/hzwctl"


def run(command, timeout):
    return subprocess.run(command, timeout=timeout, check=False).returncode == 0


def video_runtime_seconds():
    result = subprocess.run(
        ["systemctl", "show", "hangzhouwan-video.service",
         "-p", "ActiveEnterTimestampMonotonic", "--value"],
        capture_output=True, text=True, timeout=5, check=False)
    try:
        active_us = int(result.stdout.strip())
        with open("/proc/uptime", encoding="ascii") as stream:
            uptime_s = float(stream.read().split()[0])
        return max(0, uptime_s - active_us / 1000000.0)
    except Exception:
        return 0


def main():
    os.makedirs("/run/hangzhouwan", mode=0o750, exist_ok=True)
    lock = open(LOCK_PATH, "w", encoding="ascii")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 0
    lock.write(str(os.getpid()))
    lock.flush()
    alerts = AlertLog()
    doc, mqtt_config = load_config("/etc/hangzhouwan/application.yaml")
    publisher = MqttAlertPublisher(
        mqtt_config, os.environ.get("HZW_DEVICE_ID", socket.gethostname()))

    def emit(alert_type, severity, message, details=None):
        record = alerts.emit(alert_type, severity, message, details)
        publisher.publish_alert(record)

    started = time.monotonic()
    mutation_started = False
    try:
        with open("/proc/uptime", encoding="ascii") as stream:
            uptime = float(stream.read().split()[0])
        if uptime < 1800 or video_runtime_seconds() < 1800:
            emit("daily_maintenance_skipped", "info",
                 "开机或 Video 运行不足30分钟，跳过补执行")
            return 0
        if not os.path.ismount("/data"):
            raise RuntimeError("/data 未挂载")
        if shutil.disk_usage("/data").free < 2 * 1024 * 1024 * 1024:
            raise RuntimeError("/data 可用空间不足2GB")
        root_critical_mb = int(
            doc.get("supervisor", {}).get("root_disk_critical_mb", 1536))
        if shutil.disk_usage("/").free < root_critical_mb * 1024 * 1024:
            raise RuntimeError(
                f"根盘可用空间不足{root_critical_mb}MB")
        if not upstream_reachable(doc):
            raise RuntimeError("上游 RTSP/RTMP 端口不可达")

        diag_env = dict(os.environ)
        diag_env["HZW_DIAG_DIR"] = "/data/hangzhouwan/monitor/diagnostics"
        subprocess.run([HZWCTL, "collect-diagnostics"], env=diag_env,
                       timeout=30, check=False)
        subprocess.run(["systemctl", "reset-failed",
                        "hangzhouwan-business.service",
                        "hangzhouwan-video.service"], timeout=10, check=False)
        mutation_started = True
        if not run(["systemctl", "restart",
                    "hangzhouwan-business.service"], 30):
            raise RuntimeError("Business 重启失败")
        if not run([HZWCTL, "wait-business", "--timeout", "45"], 50):
            raise RuntimeError("Sidecar 验证失败")
        if not run(["systemctl", "restart",
                    "hangzhouwan-video.service"], 45):
            raise RuntimeError("Video 重启失败")
        for confirmation in range(3):
            remaining = int(180 - (time.monotonic() - started))
            if remaining <= 0 or not run(
                    [HZWCTL, "wait-health", "--timeout",
                     str(min(remaining, 90))], min(remaining, 90) + 5):
                raise RuntimeError(
                    f"A/B 严格健康验证失败（第{confirmation + 1}次）")
            if confirmation < 2:
                time.sleep(5)
        emit("daily_maintenance_completed", "info",
             "每日受控维护成功",
             {"duration_seconds": int(time.monotonic() - started)})
        return 0
    except Exception as exc:
        recovery_details = {
            "duration_seconds": int(time.monotonic() - started),
        }
        post_diagnostic = collect_diagnostics(
            "maintenance_post" if mutation_started
            else "maintenance_gate_failed")
        if post_diagnostic:
            recovery_details["post_diagnostic"] = post_diagnostic
        availability = None
        availability_details = {}
        if mutation_started:
            availability, availability_details = current_availability(doc)
            recovery_details["availability"] = availability
            recovery_details["availability_details"] = availability_details
        if (mutation_started
                and availability not in (HEALTHY, OPERATIONAL_DEGRADED)):
            stopped_cleanly = stop_services()
            reset_failed()
            recovery_details["stopped_cleanly"] = stopped_cleanly
            stopped_diagnostic = collect_diagnostics("maintenance_stopped")
            if stopped_diagnostic:
                recovery_details["stopped_diagnostic"] = stopped_diagnostic
        # 门禁失败也保留待恢复状态，但不得因此停止仍在运行的数据面。
        # /data 未挂载时不能把状态回退写入根盘，只发布本轮告警。
        if os.path.ismount("/data"):
            schedule_recovery(str(exc), recovery_details)
            arm_recovery_timer()
        degraded = availability == OPERATIONAL_DEGRADED
        emit("daily_maintenance_degraded" if degraded
             else "daily_maintenance_failed",
             "warning" if degraded else "critical", str(exc),
             recovery_details)
        return 1
    finally:
        try:
            os.unlink(LOCK_PATH)
        except FileNotFoundError:
            pass
        lock.close()


if __name__ == "__main__":
    sys.exit(main())
