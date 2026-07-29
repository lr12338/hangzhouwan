import os
import tempfile
import unittest
from unittest import mock

from services.monitoring import controlled_maintenance as maintenance


class DummyCompleted:
    returncode = 0
    stdout = ""


class DummyAlerts:
    def emit(self, alert_type, severity, message, details=None):
        return {"type": alert_type, "details": details or {}}


class DummyPublisher:
    def __init__(self, *_args, **_kwargs):
        pass

    def publish_alert(self, _record):
        pass


class ControlledMaintenanceRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.lock = os.path.join(self.temp.name, "maintenance.lock")
        self.common = [
            mock.patch.object(maintenance, "LOCK_PATH", self.lock),
            mock.patch.object(maintenance, "video_runtime_seconds",
                              return_value=3600),
            mock.patch.object(maintenance.os.path, "ismount",
                              return_value=True),
            mock.patch.object(
                maintenance.shutil, "disk_usage",
                return_value=mock.Mock(free=10 * 1024 * 1024 * 1024)),
            mock.patch.object(
                maintenance, "load_config",
                return_value=({"supervisor": {}}, {})),
            mock.patch.object(maintenance, "AlertLog",
                              return_value=DummyAlerts()),
            mock.patch.object(maintenance, "MqttAlertPublisher",
                              DummyPublisher),
            mock.patch.object(maintenance.subprocess, "run",
                              return_value=DummyCompleted()),
            mock.patch.object(maintenance.time, "sleep"),
        ]
        for patcher in self.common:
            patcher.start()

    def tearDown(self):
        for patcher in reversed(self.common):
            patcher.stop()
        self.temp.cleanup()

    def test_health_failure_stops_services_and_schedules_retry(self):
        calls = iter([True, True, True, False])
        with mock.patch.object(maintenance, "upstream_reachable",
                               return_value=True), \
                mock.patch.object(
                    maintenance, "run",
                    side_effect=lambda *_args, **_kwargs: next(calls)), \
                mock.patch.object(
                    maintenance, "collect_diagnostics",
                    return_value="/data/post.txt"), \
                mock.patch.object(maintenance, "stop_services",
                                  return_value=True) as stop, \
                mock.patch.object(maintenance, "reset_failed") as reset, \
                mock.patch.object(maintenance, "schedule_recovery") as schedule, \
                mock.patch.object(maintenance, "arm_recovery_timer") as arm:
            self.assertEqual(maintenance.main(), 1)
        stop.assert_called_once()
        reset.assert_called_once()
        schedule.assert_called_once()
        arm.assert_called_once()

    def test_preflight_failure_schedules_without_stopping_services(self):
        with mock.patch.object(maintenance, "upstream_reachable",
                               return_value=False), \
                mock.patch.object(maintenance, "collect_diagnostics",
                                  return_value="/data/gate.txt"), \
                mock.patch.object(maintenance, "stop_services") as stop, \
                mock.patch.object(maintenance, "schedule_recovery") as schedule, \
                mock.patch.object(maintenance, "arm_recovery_timer") as arm:
            self.assertEqual(maintenance.main(), 1)
        stop.assert_not_called()
        schedule.assert_called_once()
        arm.assert_called_once()


if __name__ == "__main__":
    unittest.main()
