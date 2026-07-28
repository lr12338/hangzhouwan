#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""30 秒健康监督器：告警、有限自动恢复及 MQTT QoS1 离线队列。"""

import argparse
import collections
import datetime as dt
import fcntl
import gzip
import json
import os
import shutil
import socket
import subprocess
import threading
import time
from urllib.parse import urlparse

try:
    import yaml
except ImportError:
    yaml = None

VIDEO_SOCKET = "/run/hangzhouwan/video-health.sock"
BUSINESS_SOCKET = "/run/hangzhouwan/business.sock"
MAINTENANCE_LOCK = "/run/hangzhouwan/maintenance.lock"
MONITOR_DIR = "/data/hangzhouwan/monitor"
STATE_PATH = os.path.join(MONITOR_DIR, "supervisor-state.json")
ALERT_PATH = os.path.join(MONITOR_DIR, "alerts.current.jsonl")
MQTT_SPOOL = os.path.join(MONITOR_DIR, "mqtt-offline.jsonl")


def now_ms():
    return int(time.time() * 1000)


def query_unix_health(path, timeout=3):
    if not os.path.exists(path):
        return None
    try:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(timeout)
        client.connect(path)
        client.sendall(b'{"action":"health"}\n')
        data = b""
        while b"\n" not in data:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        client.close()
        return json.loads(data.split(b"\n", 1)[0].decode("utf-8"))
    except Exception:
        return None


def query_video(timeout=3):
    return query_unix_health(VIDEO_SOCKET, timeout)


def query_business(timeout=3):
    return query_unix_health(BUSINESS_SOCKET, timeout)


class RecoveryPolicy:
    """纯策略状态，便于覆盖连续失败、冷却和每日上限单元测试。"""

    def __init__(self, cooldown_seconds=1800, daily_limit=2):
        self.cooldown_seconds = cooldown_seconds
        self.daily_limit = daily_limit
        self.failed_streak = 0
        self.socket_lost_since = 0
        self.recoveries = collections.deque()

    def observe(self, status, socket_ok, epoch_seconds):
        if socket_ok:
            self.socket_lost_since = 0
        elif not self.socket_lost_since:
            self.socket_lost_since = epoch_seconds
        if status == "FAILED":
            self.failed_streak += 1
        else:
            self.failed_streak = 0
        while self.recoveries and self.recoveries[0] < epoch_seconds - 86400:
            self.recoveries.popleft()

    def should_recover(self, epoch_seconds):
        socket_failed = (
            self.socket_lost_since
            and epoch_seconds - self.socket_lost_since >= 90
        )
        if self.failed_streak < 3 and not socket_failed:
            return False, "failure_not_confirmed"
        if len(self.recoveries) >= self.daily_limit:
            return False, "daily_limit"
        if self.recoveries and epoch_seconds - self.recoveries[-1] < self.cooldown_seconds:
            return False, "cooldown"
        return True, "confirmed_failure"

    def record_recovery(self, epoch_seconds):
        self.recoveries.append(epoch_seconds)
        self.failed_streak = 0
        self.socket_lost_since = 0

    def to_dict(self):
        return {
            "failed_streak": self.failed_streak,
            "socket_lost_since": self.socket_lost_since,
            "recoveries": list(self.recoveries),
        }

    def restore(self, value):
        if not isinstance(value, dict):
            return
        self.failed_streak = int(value.get("failed_streak", 0))
        self.socket_lost_since = int(value.get("socket_lost_since", 0))
        self.recoveries = collections.deque(
            int(item) for item in value.get("recoveries", []))


def save_policy(policy):
    temporary = STATE_PATH + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(policy.to_dict(), stream, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, STATE_PATH)


class AlertLog:
    def __init__(self, directory=MONITOR_DIR, retention_days=7):
        self.directory = directory
        self.path = os.path.join(directory, "alerts.current.jsonl")
        self.lock_path = os.path.join(directory, ".alerts.lock")
        self.retention_days = retention_days
        os.makedirs(directory, mode=0o750, exist_ok=True)

    def rotate(self):
        if not os.path.exists(self.path):
            return
        stat = os.stat(self.path)
        day_changed = dt.date.fromtimestamp(stat.st_mtime) != dt.date.today()
        if stat.st_size < 20 * 1024 * 1024 and not day_changed:
            return
        stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
        target = os.path.join(self.directory, f"alerts-{stamp}.jsonl.gz")
        with open(self.path, "rb") as src, gzip.open(target + ".tmp", "wb", 1) as dst:
            shutil.copyfileobj(src, dst)
        os.replace(target + ".tmp", target)
        os.unlink(self.path)
        cutoff = time.time() - self.retention_days * 86400
        for name in os.listdir(self.directory):
            if name.startswith("alerts-") and name.endswith(".jsonl.gz"):
                path = os.path.join(self.directory, name)
                if os.stat(path).st_mtime < cutoff:
                    os.unlink(path)

    def emit(self, alert_type, severity, message, details=None):
        with open(self.lock_path, "a+", encoding="ascii") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.rotate()
            record = {
                "schema_version": 1,
                "timestamp_ms": now_ms(),
                "type": alert_type,
                "severity": severity,
                "message": message,
                "details": details or {},
            }
            with open(self.path, "a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=False,
                                        separators=(",", ":")) + "\n")
                stream.flush()
                os.fsync(stream.fileno())
        return record


class MqttAlertPublisher:
    """监督器独立 MQTT 发布器；视频热路径不等待网络。"""

    MAX_RECORDS = 1000
    MAX_BYTES = 10 * 1024 * 1024

    def __init__(self, config, device_id):
        self.config = config
        self.device_id = device_id
        self.state_topic = f"hangzhouwan/{device_id}/state"
        self.events_topic = f"hangzhouwan/{device_id}/events"
        self.connected = False
        self.client = None
        self.queue = collections.deque()
        self.queue_bytes = 0
        self.lock = threading.Lock()
        self.spool_lock_path = MQTT_SPOOL + ".lock"
        self._load_spool()
        self._connect()

    def _load_spool(self):
        os.makedirs(MONITOR_DIR, mode=0o750, exist_ok=True)
        try:
            with open(self.spool_lock_path, "a+", encoding="ascii") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                self._reload_spool_locked()
        except Exception:
            pass

    def _reload_spool_locked(self):
        self.queue.clear()
        self.queue_bytes = 0
        try:
            with open(MQTT_SPOOL, encoding="utf-8") as stream:
                for line in stream:
                    self._append(json.loads(line))
        except FileNotFoundError:
            pass

    def _append(self, item):
        encoded = json.dumps(item, ensure_ascii=False, separators=(",", ":"))
        size = len(encoded.encode("utf-8")) + 1
        self.queue.append((item, size))
        self.queue_bytes += size
        while (len(self.queue) > self.MAX_RECORDS or
               self.queue_bytes > self.MAX_BYTES):
            _, removed = self.queue.popleft()
            self.queue_bytes -= removed

    def _persist_locked(self):
        temporary = MQTT_SPOOL + ".tmp"
        with open(temporary, "w", encoding="utf-8") as stream:
            for item, _ in self.queue:
                stream.write(json.dumps(item, ensure_ascii=False,
                                        separators=(",", ":")) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, MQTT_SPOOL)

    def _spool(self, item):
        with self.lock:
            with open(self.spool_lock_path, "a+", encoding="ascii") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                self._reload_spool_locked()
                self._append(item)
                self._persist_locked()

    def _connect(self):
        host = self.config.get("host", "")
        if not host:
            return
        try:
            import paho.mqtt.client as mqtt
            self.client = mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                client_id=(
                    self.config.get("client_id") or self.device_id
                ) + "-monitor",
            )
            if self.config.get("username"):
                self.client.username_pw_set(
                    self.config["username"], self.config.get("password", ""))
            self.client.on_connect = self._on_connect
            self.client.on_disconnect = self._on_disconnect
            self.client.connect_async(host, self.config.get("port", 1883),
                                      self.config.get("keepalive", 60))
            self.client.loop_start()
        except Exception:
            self.client = None

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        self.connected = int(reason_code) == 0
        if self.connected:
            self.flush()

    def _on_disconnect(self, client, userdata, flags, reason_code=None,
                       properties=None):
        self.connected = False

    def publish_state(self, health):
        payload = {
            "schema_version": 1,
            "timestamp_ms": now_ms(),
            "device_id": self.device_id,
            "health": health,
        }
        self._publish(self.state_topic, payload, retain=True, spool=False)

    def publish_alert(self, record):
        self._publish(self.events_topic, record, retain=False, spool=True)

    def _publish(self, topic, payload, retain, spool):
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        if self.client and self.connected:
            try:
                info = self.client.publish(topic, encoded, qos=1, retain=retain)
                if info.rc == 0:
                    return
            except Exception:
                self.connected = False
        if spool:
            self._spool({"topic": topic, "payload": payload,
                         "retain": retain})

    def flush(self):
        if not self.client or not self.connected:
            return
        with self.lock:
            with open(self.spool_lock_path, "a+", encoding="ascii") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                self._reload_spool_locked()
                while self.queue:
                    item, size = self.queue[0]
                    encoded = json.dumps(item["payload"], ensure_ascii=False,
                                         separators=(",", ":"))
                    info = self.client.publish(
                        item["topic"], encoded, qos=1,
                        retain=item.get("retain", False))
                    if info.rc != 0:
                        break
                    self.queue.popleft()
                    self.queue_bytes -= size
                self._persist_locked()


def load_config(path):
    if not yaml:
        return {}, {}
    with open(path, encoding="utf-8") as stream:
        doc = yaml.safe_load(stream) or {}
    mqtt = doc.get("mqtt", {})
    resolved = {
        "host": os.environ.get(mqtt.get("host_env", ""), ""),
        "port": int(mqtt.get("port", 1883)),
        "client_id": os.environ.get(mqtt.get("client_id_env", ""), ""),
        "username": os.environ.get(mqtt.get("username_env", ""), ""),
        "password": os.environ.get(mqtt.get("password_env", ""), ""),
        "keepalive": int(mqtt.get("keepalive", 60)),
    }
    return doc, resolved


def upstream_reachable(doc):
    for stream in doc.get("streams", []):
        if not stream.get("enabled"):
            continue
        for key, default_port in (("input_url_env", 554),
                                  ("output_url_env", 1935)):
            url = os.environ.get(stream.get(key, ""), "")
            parsed = urlparse(url)
            if not parsed.hostname:
                return False
            try:
                with socket.create_connection(
                        (parsed.hostname, parsed.port or default_port), timeout=3):
                    pass
            except OSError:
                return False
    return True


def recovery_allowed(doc):
    if os.path.exists(MAINTENANCE_LOCK):
        return False, "maintenance_running"
    if not os.path.ismount("/data"):
        return False, "data_not_mounted"
    if shutil.disk_usage("/data").free < 1024 * 1024 * 1024:
        return False, "disk_critical"
    if not upstream_reachable(doc):
        return False, "upstream_unreachable"
    return True, "ok"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/etc/hangzhouwan/application.yaml")
    parser.add_argument("--interval", type=int, default=30)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()

    os.makedirs(MONITOR_DIR, mode=0o750, exist_ok=True)
    doc, mqtt_config = load_config(args.config)
    device_id = os.environ.get("HZW_DEVICE_ID", socket.gethostname())
    alerts = AlertLog()
    publisher = MqttAlertPublisher(mqtt_config, device_id)
    supervisor_cfg = doc.get("supervisor", {})
    policy = RecoveryPolicy(
        int(supervisor_cfg.get("cooldown_seconds", 1800)),
        int(supervisor_cfg.get("daily_recovery_limit", 2)),
    )
    try:
        with open(STATE_PATH, encoding="utf-8") as stream:
            policy.restore(json.load(stream))
    except Exception:
        pass
    last_signature = None
    last_mqtt_connected = None
    last_active_alerts = set()
    last_recovery_notice = None

    while True:
        epoch = int(time.time())
        health = query_video()
        business_health = query_business()
        socket_ok = health is not None
        status = health.get("status", "FAILED") if health else "FAILED"
        policy.observe(status, socket_ok, epoch)
        save_policy(policy)
        signature = (status, health.get("health_reason", "") if health
                     else "video health socket unavailable")
        if status != "HEALTHY" and signature != last_signature:
            record = alerts.emit(
                "service_failed" if status == "FAILED" else "service_degraded",
                "critical" if status == "FAILED" else "warning",
                signature[1] or status, health or {})
            publisher.publish_alert(record)
        elif (status == "HEALTHY" and last_signature and
              last_signature[0] != "HEALTHY"):
            record = alerts.emit(
                "service_recovered", "info", "Video 全链路恢复健康",
                health or {})
            publisher.publish_alert(record)
        last_signature = signature
        active_alerts = set((health or {}).get("active_alerts", []))
        for active in sorted(active_alerts - last_active_alerts):
            severity = (
                "critical"
                if "fatal" in active or "UNAVAILABLE" in active
                else "warning"
            )
            record = alerts.emit(
                active, severity, f"Video 活动告警: {active}", health or {})
            publisher.publish_alert(record)
        last_active_alerts = active_alerts
        mqtt_connected = bool(
            business_health and business_health.get("mqtt_connected"))
        if mqtt_config.get("host") and mqtt_connected != last_mqtt_connected:
            if not mqtt_connected:
                record = alerts.emit(
                    "mqtt_disconnected", "warning",
                    "现有 MQTT 链路中断", business_health or {})
                publisher.publish_alert(record)
            elif last_mqtt_connected is False:
                record = alerts.emit(
                    "mqtt_recovered", "info", "现有 MQTT 链路恢复",
                    business_health or {})
                publisher.publish_alert(record)
        last_mqtt_connected = mqtt_connected
        publisher.publish_state(health or {"status": "FAILED",
                                           "reason": "socket unavailable"})
        publisher.flush()

        recover, reason = policy.should_recover(epoch)
        recovery_notice = None
        if recover:
            allowed, gate = recovery_allowed(doc)
            if allowed:
                env = dict(os.environ)
                env["HZW_DIAG_DIR"] = os.path.join(MONITOR_DIR, "diagnostics")
                returncode = -1
                try:
                    subprocess.run(
                        ["/opt/hangzhouwan/current/bin/hzwctl",
                         "collect-diagnostics"], env=env, timeout=30,
                        check=False)
                    result = subprocess.run(
                        ["/bin/systemctl", "restart",
                         "hangzhouwan-video.service"], timeout=60, check=False)
                    returncode = result.returncode
                except (OSError, subprocess.TimeoutExpired):
                    returncode = -1
                policy.record_recovery(epoch)
                save_policy(policy)
                record = alerts.emit(
                    "automatic_recovery",
                    "warning" if returncode == 0 else "critical",
                    "Video 已执行一次受限恢复" if returncode == 0
                    else "Video 自动恢复失败",
                    {"returncode": returncode})
                publisher.publish_alert(record)
            elif gate not in ("maintenance_running",):
                recovery_notice = ("automatic_recovery_inhibited", gate)
        elif reason == "daily_limit":
            recovery_notice = ("automatic_recovery_locked", reason)

        if recovery_notice != last_recovery_notice and recovery_notice:
            alert_type, notice_reason = recovery_notice
            if alert_type == "automatic_recovery_locked":
                record = alerts.emit(
                    alert_type, "critical",
                    "24小时自动恢复次数已达上限，要求人工处理",
                    {"limit": policy.daily_limit})
            else:
                record = alerts.emit(
                    alert_type, "critical", "自动恢复被安全门禁禁止",
                    {"reason": notice_reason})
            publisher.publish_alert(record)
        last_recovery_notice = recovery_notice

        if args.once:
            break
        time.sleep(max(args.interval, 5))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
