import json
import os
import sys
import tempfile
import unittest
from unittest import mock

from services.monitoring import maintenance_recovery as recovery
from services.monitoring.availability import OPERATIONAL_DEGRADED, UNAVAILABLE


class DummyPublisher:
    def publish_alert(self, _record):
        pass

    def flush(self):
        pass


class DummyAlerts:
    def emit(self, alert_type, severity, message, details):
        return {
            "type": alert_type,
            "severity": severity,
            "message": message,
            "details": details,
        }


class MaintenanceRecoveryStateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.state_path = os.path.join(self.temp.name, "state.json")
        self.lock_path = os.path.join(self.temp.name, "maintenance.lock")
        self.patches = [
            mock.patch.object(recovery, "STATE_PATH", self.state_path),
            mock.patch.object(recovery, "LOCK_PATH", self.lock_path),
        ]
        for patcher in self.patches:
            patcher.start()

    def tearDown(self):
        for patcher in reversed(self.patches):
            patcher.stop()
        self.temp.cleanup()

    def test_schedule_is_atomic_and_preserves_attempt_count(self):
        state = recovery.schedule_recovery("first", epoch=100)
        self.assertEqual(state["restart_attempts"], 0)
        state["restart_attempts"] = 4
        with open(self.state_path, "w", encoding="utf-8") as stream:
            json.dump(state, stream)
        updated = recovery.schedule_recovery("again", epoch=200)
        self.assertEqual(updated["first_failed_at"], 100)
        self.assertEqual(updated["restart_attempts"], 4)
        self.assertEqual(updated["next_attempt_at"], 200 + 1800)

    def _run_main(self, gate, healthy=True):
        recovery.schedule_recovery("failure", epoch=100)
        argv = ["maintenance_recovery"]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(
                    recovery, "_publisher",
                    return_value=({}, DummyAlerts(), DummyPublisher())), \
                mock.patch.object(recovery, "recovery_gate",
                                  return_value=gate), \
                mock.patch.object(recovery, "current_availability",
                                  return_value=(UNAVAILABLE, {})), \
                mock.patch.object(recovery, "collect_diagnostics",
                                  return_value="/data/diag.txt"), \
                mock.patch.object(recovery, "stop_services",
                                  return_value=True) as stop, \
                mock.patch.object(recovery, "reset_failed",
                                  return_value=True), \
                mock.patch.object(recovery, "_strict_start",
                                  return_value=(healthy, "failed")) as start:
            result = recovery.main()
        return result, stop, start

    def test_unreachable_gate_does_not_restart(self):
        result, stop, start = self._run_main(
            (False, "upstream_unreachable"))
        self.assertEqual(result, 0)
        stop.assert_not_called()
        start.assert_not_called()
        self.assertTrue(os.path.exists(self.state_path))

    def test_success_clears_pending_state(self):
        result, stop, start = self._run_main((True, "ok"), healthy=True)
        self.assertEqual(result, 0)
        stop.assert_called_once()
        start.assert_called_once()
        self.assertFalse(os.path.exists(self.state_path))

    def test_failure_keeps_state_for_unlimited_retry(self):
        result, stop, start = self._run_main((True, "ok"), healthy=False)
        self.assertEqual(result, 1)
        self.assertGreaterEqual(stop.call_count, 2)
        start.assert_called_once()
        state = recovery.load_recovery_state()
        self.assertEqual(state["status"], "PENDING")
        self.assertEqual(state["restart_attempts"], 1)

    def test_operational_degraded_does_not_restart(self):
        recovery.schedule_recovery("failure", epoch=100)
        details = {"business_healthy": True, "operational_streams": ["A"]}
        with mock.patch.object(sys, "argv", ["maintenance_recovery"]), \
                mock.patch.object(
                    recovery, "_publisher",
                    return_value=({}, DummyAlerts(), DummyPublisher())), \
                mock.patch.object(recovery, "recovery_gate",
                                  return_value=(True, "ok")), \
                mock.patch.object(recovery, "current_availability",
                                  return_value=(OPERATIONAL_DEGRADED, details)), \
                mock.patch.object(recovery, "stop_services") as stop, \
                mock.patch.object(recovery, "_strict_start") as start:
            self.assertEqual(recovery.main(), 0)
        stop.assert_not_called()
        start.assert_not_called()
        state = recovery.load_recovery_state()
        self.assertEqual(state["last_gate"], "operational_degraded")

    def test_business_only_failure_does_not_restart_video(self):
        recovery.schedule_recovery("failure", epoch=100)
        degraded = (OPERATIONAL_DEGRADED,
                    {"business_healthy": False, "operational_streams": ["A"]})
        with mock.patch.object(sys, "argv", ["maintenance_recovery"]), \
                mock.patch.object(
                    recovery, "_publisher",
                    return_value=({}, DummyAlerts(), DummyPublisher())), \
                mock.patch.object(recovery, "recovery_gate",
                                  return_value=(True, "ok")), \
                mock.patch.object(recovery, "current_availability",
                                  return_value=degraded), \
                mock.patch.object(recovery, "recover_business_only",
                                  return_value=False) as business, \
                mock.patch.object(recovery, "stop_services") as stop:
            self.assertEqual(recovery.main(), 0)
        business.assert_called_once()
        stop.assert_not_called()

    def test_no_pending_state_is_noop(self):
        with mock.patch.object(sys, "argv", ["maintenance_recovery"]), \
                mock.patch.object(recovery, "_strict_start") as start:
            self.assertEqual(recovery.main(), 0)
        start.assert_not_called()

    def test_pending_state_before_due_time_is_noop(self):
        recovery.schedule_recovery("failure", epoch=1000)
        with mock.patch.object(sys, "argv", ["maintenance_recovery"]), \
                mock.patch.object(recovery.time, "time",
                                  return_value=1001), \
                mock.patch.object(recovery, "_strict_start") as start:
            self.assertEqual(recovery.main(), 0)
        start.assert_not_called()

    def test_root_disk_gate_blocks_restart(self):
        def usage(path):
            free = (10 * 1024 * 1024 * 1024 if path == "/data"
                    else 1024 * 1024 * 1024)
            return mock.Mock(free=free)

        with mock.patch.object(recovery.os.path, "ismount",
                               return_value=True), \
                mock.patch.object(recovery.shutil, "disk_usage",
                                  side_effect=usage), \
                mock.patch.object(recovery, "upstream_reachable") as upstream:
            allowed, reason = recovery.recovery_gate({"supervisor": {}})
        self.assertFalse(allowed)
        self.assertEqual(reason, "root_disk_critical")
        upstream.assert_not_called()


if __name__ == "__main__":
    unittest.main()
