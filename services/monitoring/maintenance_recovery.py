#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""维护失败后的持久、门禁化、每半小时恢复 worker。"""

import argparse
import fcntl
import json
import os
import shutil
import socket
import subprocess
import time

from .supervisor import (
    AlertLog,
    MqttAlertPublisher,
    load_config,
    upstream_reachable,
    query_business,
    query_video,
)
from .availability import (
    HEALTHY,
    OPERATIONAL_DEGRADED,
    classify_availability,
)

MONITOR_DIR = "/data/hangzhouwan/monitor"
STATE_PATH = os.path.join(MONITOR_DIR, "maintenance-recovery.json")
LOCK_PATH = "/run/hangzhouwan/maintenance.lock"
HZWCTL = "/opt/hangzhouwan/current/bin/hzwctl"
VIDEO_SERVICE = "hangzhouwan-video.service"
BUSINESS_SERVICE = "hangzhouwan-business.service"
RECOVERY_TIMER = "hangzhouwan-maintenance-recovery.timer"
RETRY_SECONDS = 1800
ALERT_REPEAT_SECONDS = 6 * 3600
ROOT_CRITICAL_BYTES = 1536 * 1024 * 1024
DATA_REQUIRED_BYTES = 2 * 1024 * 1024 * 1024


def _atomic_write(path, value):
    os.makedirs(os.path.dirname(path), mode=0o750, exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def load_recovery_state():
    try:
        with open(STATE_PATH, encoding="utf-8") as stream:
            value = json.load(stream)
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def schedule_recovery(reason, details=None, epoch=None):
    """创建或更新持久恢复状态；实际重启次数只由 worker 增加。"""
    now = int(time.time() if epoch is None else epoch)
    state = load_recovery_state() or {}
    state.update({
        "status": "PENDING",
        "reason": str(reason),
        "details": details or {},
        "first_failed_at": int(state.get("first_failed_at", now)),
        "last_checked_at": now,
        "last_attempt_at": int(state.get("last_attempt_at", 0)),
        "restart_attempts": int(state.get("restart_attempts", 0)),
        "last_gate": str(state.get("last_gate", "pending")),
        "last_diagnostic": str(state.get("last_diagnostic", "")),
        "last_alert_at": int(state.get("last_alert_at", 0)),
        "next_attempt_at": now + RETRY_SECONDS,
    })
    _atomic_write(STATE_PATH, state)
    return state


def collect_diagnostics(phase):
    os.makedirs(os.path.join(MONITOR_DIR, "diagnostics"),
                mode=0o750, exist_ok=True)
    env = dict(os.environ)
    env["HZW_DIAG_DIR"] = os.path.join(MONITOR_DIR, "diagnostics")
    env["HZW_DIAG_PHASE"] = phase
    try:
        result = subprocess.run(
            [HZWCTL, "collect-diagnostics"], env=env, timeout=45,
            capture_output=True, text=True, check=False)
        for line in reversed(result.stdout.splitlines()):
            if line.startswith("DIAGNOSTIC_PATH="):
                return line.split("=", 1)[1]
    except (OSError, subprocess.TimeoutExpired):
        pass
    return ""


def _run(command, timeout):
    try:
        return subprocess.run(
            command, timeout=timeout, check=False).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _service_stopped(service):
    try:
        result = subprocess.run(
            ["systemctl", "show", service,
             "-p", "ActiveState", "-p", "MainPID", "--value"],
            capture_output=True, text=True, timeout=5, check=False)
        values = [item.strip() for item in result.stdout.splitlines()]
        return "active" not in values and "activating" not in values and (
            "0" in values or not any(item.isdigit() for item in values))
    except (OSError, subprocess.TimeoutExpired):
        return False


def _service_cgroup_empty(service):
    try:
        result = subprocess.run(
            ["systemctl", "show", service, "-p", "ControlGroup", "--value"],
            capture_output=True, text=True, timeout=5, check=False)
        cgroup = result.stdout.strip()
        if not cgroup:
            return True
        base = os.path.join("/sys/fs/cgroup", cgroup.lstrip("/"))
        for name in ("cgroup.procs", "cgroup.threads", "tasks"):
            path = os.path.join(base, name)
            try:
                with open(path, encoding="ascii") as stream:
                    if stream.read().strip():
                        return False
            except FileNotFoundError:
                continue
        return True
    except (OSError, subprocess.TimeoutExpired):
        return False


def _residual_service_processes():
    residual = []
    markers = ("dual_stream_app", "services.business_enrichment.app")
    try:
        entries = os.listdir("/proc")
    except OSError:
        return ["proc_unavailable"]
    for entry in entries:
        if not entry.isdigit() or int(entry) == os.getpid():
            continue
        try:
            with open(
                    os.path.join("/proc", entry, "cmdline"),
                    "rb") as stream:
                command = stream.read().replace(b"\0", b" ").decode(
                    "utf-8", "replace")
        except OSError:
            continue
        if any(marker in command for marker in markers):
            residual.append(entry)
    return residual


def stop_services():
    """先停 Video 再停 Business，并确认进程、线程和控制组已收口。"""
    video_ok = _run(["systemctl", "stop", VIDEO_SERVICE], 60)
    business_ok = _run(["systemctl", "stop", BUSINESS_SERVICE], 45)
    return (video_ok and business_ok and
            _service_stopped(VIDEO_SERVICE) and
            _service_stopped(BUSINESS_SERVICE) and
            _service_cgroup_empty(VIDEO_SERVICE) and
            _service_cgroup_empty(BUSINESS_SERVICE) and
            not _residual_service_processes())


def reset_failed():
    return _run(
        ["systemctl", "reset-failed", BUSINESS_SERVICE, VIDEO_SERVICE], 10)


def arm_recovery_timer():
    """保证 timer 已启动；具体到期时间由持久状态和 worker 双重约束。"""
    return _run(["systemctl", "start", RECOVERY_TIMER], 10)


def recovery_gate(doc):
    if not os.path.ismount("/data"):
        return False, "data_not_mounted"
    if shutil.disk_usage("/data").free < DATA_REQUIRED_BYTES:
        return False, "data_disk_critical"
    supervisor = doc.get("supervisor", {})
    root_critical = int(
        supervisor.get("root_disk_critical_mb",
                       ROOT_CRITICAL_BYTES // (1024 * 1024)))
    if shutil.disk_usage("/").free < root_critical * 1024 * 1024:
        return False, "root_disk_critical"
    if not upstream_reachable(doc):
        return False, "upstream_unreachable"
    return True, "ok"


def current_availability(doc):
    return classify_availability(query_video(), query_business(), doc)


def recover_business_only():
    """Recover enrichment without interrupting a usable video data plane."""
    _run(["systemctl", "reset-failed", BUSINESS_SERVICE], 10)
    if not _run(["systemctl", "restart", BUSINESS_SERVICE], 45):
        return False
    return _run([HZWCTL, "wait-business", "--timeout", "45"], 50)


def _strict_start():
    if not _run(["systemctl", "start", BUSINESS_SERVICE], 45):
        return False, "business_start_failed"
    if not _run([HZWCTL, "wait-business", "--timeout", "45"], 50):
        return False, "business_health_failed"
    if not _run(["systemctl", "start", VIDEO_SERVICE], 60):
        return False, "video_start_failed"
    for confirmation in range(3):
        if not _run([HZWCTL, "wait-health", "--timeout", "90"], 95):
            return False, "video_health_confirmation_{}_failed".format(
                confirmation + 1)
        if confirmation < 2:
            time.sleep(5)
    return True, "ok"


def _publisher(config_path):
    doc, mqtt_config = load_config(config_path)
    publisher = MqttAlertPublisher(
        mqtt_config, os.environ.get("HZW_DEVICE_ID", socket.gethostname()))
    return doc, AlertLog(), publisher


def _emit(alerts, publisher, alert_type, severity, message, details):
    record = alerts.emit(alert_type, severity, message, details)
    publisher.publish_alert(record)
    publisher.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/etc/hangzhouwan/application.yaml")
    args = parser.parse_args()

    state = load_recovery_state()
    if not state or state.get("status") != "PENDING":
        return 0
    now = int(time.time())
    if now < int(state.get("next_attempt_at", 0)):
        return 0

    os.makedirs("/run/hangzhouwan", mode=0o750, exist_ok=True)
    lock = open(LOCK_PATH, "w", encoding="ascii")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        return 0
    lock.write(str(os.getpid()))
    lock.flush()

    try:
        doc, alerts, publisher = _publisher(args.config)
        now = int(time.time())
        supervisor_cfg = doc.get("supervisor", {})
        retry_seconds = int(
            supervisor_cfg.get("recovery_retry_seconds", RETRY_SECONDS))
        alert_repeat_seconds = int(
            supervisor_cfg.get(
                "recovery_alert_repeat_seconds", ALERT_REPEAT_SECONDS))
        allowed, gate = recovery_gate(doc)
        prior_gate = state.get("last_gate")
        state["last_checked_at"] = now
        state["last_gate"] = gate
        state["next_attempt_at"] = now + retry_seconds
        if not allowed:
            should_alert = (
                gate != prior_gate or
                now - int(state.get("last_alert_at", 0)) >=
                alert_repeat_seconds
            )
            if should_alert:
                state["last_alert_at"] = now
                _emit(alerts, publisher, "maintenance_recovery_inhibited",
                      "critical", "维护恢复被安全门禁禁止",
                      {"gate": gate,
                       "restart_attempts": state.get("restart_attempts", 0)})
            _atomic_write(STATE_PATH, state)
            return 0

        availability, availability_details = current_availability(doc)
        state["availability"] = availability
        state["availability_details"] = availability_details
        if availability == HEALTHY:
            _emit(alerts, publisher, "maintenance_recovery_completed", "info",
                  "Business 与 Video 已恢复严格健康",
                  {"restart_attempts": state.get("restart_attempts", 0),
                   "first_failed_at": state.get("first_failed_at")})
            try:
                os.unlink(STATE_PATH)
            except FileNotFoundError:
                pass
            return 0
        if availability == OPERATIONAL_DEGRADED:
            if not availability_details.get("business_healthy"):
                recover_business_only()
                availability, availability_details = current_availability(doc)
                state["availability"] = availability
                state["availability_details"] = availability_details
                if availability == HEALTHY:
                    _emit(
                        alerts, publisher, "maintenance_recovery_completed",
                        "info", "Business 与 Video 已恢复严格健康",
                        {"restart_attempts": state.get("restart_attempts", 0),
                         "first_failed_at": state.get("first_failed_at")})
                    try:
                        os.unlink(STATE_PATH)
                    except FileNotFoundError:
                        pass
                    return 0
            should_alert = (
                prior_gate != "operational_degraded"
                or now - int(state.get("last_alert_at", 0)) >=
                alert_repeat_seconds
            )
            state["last_gate"] = "operational_degraded"
            if should_alert:
                state["last_alert_at"] = now
                _emit(
                    alerts, publisher, "maintenance_recovery_degraded",
                    "warning", "数据面降级可用，保持运行并继续复查",
                    availability_details)
            _atomic_write(STATE_PATH, state)
            return 0

        diagnostic = collect_diagnostics("recovery_pre")
        if diagnostic:
            state["last_diagnostic"] = diagnostic
        state["last_attempt_at"] = now
        state["restart_attempts"] = int(state.get("restart_attempts", 0)) + 1
        _atomic_write(STATE_PATH, state)

        clean = stop_services()
        reset_failed()
        stopped_diagnostic = collect_diagnostics("recovery_stopped")
        if stopped_diagnostic:
            state["last_diagnostic"] = stopped_diagnostic
            _atomic_write(STATE_PATH, state)
        if clean:
            healthy, reason = _strict_start()
        else:
            healthy, reason = False, "residual_process_or_stop_failed"

        if healthy:
            _emit(alerts, publisher, "maintenance_recovery_completed", "info",
                  "Business 与 Video 已恢复严格健康",
                  {"restart_attempts": state["restart_attempts"],
                   "first_failed_at": state.get("first_failed_at")})
            try:
                os.unlink(STATE_PATH)
            except FileNotFoundError:
                pass
            return 0

        post = collect_diagnostics("recovery_post")
        availability, availability_details = current_availability(doc)
        if availability == HEALTHY:
            _emit(alerts, publisher, "maintenance_recovery_completed", "info",
                  "Business 与 Video 已恢复严格健康",
                  {"restart_attempts": state["restart_attempts"],
                   "first_failed_at": state.get("first_failed_at")})
            try:
                os.unlink(STATE_PATH)
            except FileNotFoundError:
                pass
            return 0
        if availability != OPERATIONAL_DEGRADED:
            stop_services()
            reset_failed()
        state["reason"] = reason
        state["availability"] = availability
        state["availability_details"] = availability_details
        state["last_gate"] = (
            "operational_degraded" if availability == OPERATIONAL_DEGRADED
            else "restart_failed")
        state["last_checked_at"] = int(time.time())
        state["next_attempt_at"] = state["last_checked_at"] + retry_seconds
        if post:
            state["last_diagnostic"] = post
        state["last_alert_at"] = state["last_checked_at"]
        _atomic_write(STATE_PATH, state)
        degraded = availability == OPERATIONAL_DEGRADED
        _emit(alerts, publisher,
              "maintenance_recovery_degraded" if degraded
              else "maintenance_recovery_failed",
              "warning" if degraded else "critical",
              "恢复后数据面降级可用，保持运行并继续复查" if degraded
              else "本轮完整恢复失败，30分钟后重试",
              {"reason": reason,
               "restart_attempts": state["restart_attempts"],
               "diagnostic": state.get("last_diagnostic", ""),
               "availability": availability_details})
        return 0 if degraded else 1
    finally:
        try:
            os.unlink(LOCK_PATH)
        except FileNotFoundError:
            pass
        lock.close()


if __name__ == "__main__":
    raise SystemExit(main())
