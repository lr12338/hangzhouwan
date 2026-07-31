import unittest

from services.monitoring.supervisor import RecoveryPolicy, mqtt_reason_code_value


class RecoveryPolicyTest(unittest.TestCase):
    def test_three_failed_samples_trigger_once(self):
        policy = RecoveryPolicy(cooldown_seconds=1800, daily_limit=2)
        for epoch in (100, 130):
            policy.observe("FAILED", True, epoch)
            self.assertFalse(policy.should_recover(epoch)[0])
        policy.observe("FAILED", True, 160)
        self.assertTrue(policy.should_recover(160)[0])
        policy.record_recovery(160)
        self.assertFalse(policy.should_recover(170)[0])

    def test_socket_loss_needs_ninety_seconds(self):
        policy = RecoveryPolicy()
        policy.observe("UNKNOWN", False, 100)
        self.assertFalse(policy.should_recover(189)[0])
        policy.observe("UNKNOWN", False, 190)
        self.assertTrue(policy.should_recover(190)[0])

    def test_cooldown_and_daily_limit(self):
        policy = RecoveryPolicy(cooldown_seconds=1800, daily_limit=2)
        policy.recoveries.extend([100, 2000])
        policy.failed_streak = 3
        allowed, reason = policy.should_recover(2100)
        self.assertFalse(allowed)
        self.assertEqual(reason, "daily_limit")
        policy.should_recover(90000)  # 淘汰超过24小时的记录由 observe 完成
        policy.observe("FAILED", True, 90000)
        policy.observe("FAILED", True, 90030)
        policy.observe("FAILED", True, 90060)
        allowed, reason = policy.should_recover(90060)
        self.assertTrue(allowed)
        self.assertEqual(reason, "confirmed_failure")

    def test_paho_v1_and_v2_reason_codes(self):
        class ReasonCode:
            value = 0

        self.assertEqual(mqtt_reason_code_value(0), 0)
        self.assertEqual(mqtt_reason_code_value(ReasonCode()), 0)
        self.assertEqual(mqtt_reason_code_value(object()), -1)


if __name__ == "__main__":
    unittest.main()
