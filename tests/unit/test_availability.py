import unittest

from services.monitoring.availability import (
    HEALTHY,
    OPERATIONAL_DEGRADED,
    UNAVAILABLE,
    classify_availability,
)


CONFIG = {
    "streams": [{"id": "A", "enabled": True},
                {"id": "B", "enabled": True}],
    "health": {"stream_healthy_fps": 9, "stream_degraded_fps": 5,
               "inference_healthy_fps": 4},
}
BUSINESS = {"status": "HEALTHY", "model_a_loaded": True,
            "model_b_loaded": True}


def stream(output, inference, connected=True):
    return {"lifecycle": "RUNNING", "rtsp_connected": connected,
            "rtmp_connected": connected, "output_fps": output,
            "inference_fps": inference}


def video(status, a, b, fatal=False):
    return {"status": status, "business_link": "HEALTHY",
            "resource": {"fatal": fatal}, "storage": {"state": "OK"},
            "streams": {"A": a, "B": b}}


class AvailabilityTest(unittest.TestCase):
    def test_both_strictly_healthy(self):
        state, details = classify_availability(
            video("HEALTHY", stream(10, 5), stream(9.2, 4.2)),
            BUSINESS, CONFIG)
        self.assertEqual(state, HEALTHY)
        self.assertEqual(details["strict_streams"], ["A", "B"])

    def test_one_usable_stream_keeps_data_plane_operational(self):
        state, details = classify_availability(
            video("DEGRADED", stream(10, 5), stream(0, 0, False)),
            BUSINESS, CONFIG)
        self.assertEqual(state, OPERATIONAL_DEGRADED)
        self.assertEqual(details["operational_streams"], ["A"])

    def test_below_strict_fps_is_operational_degraded(self):
        state, _ = classify_availability(
            video("DEGRADED", stream(10, 5), stream(8, 3.8)),
            BUSINESS, CONFIG)
        self.assertEqual(state, OPERATIONAL_DEGRADED)

    def test_all_streams_unavailable(self):
        state, _ = classify_availability(
            video("FAILED", stream(0, 0, False), stream(0, 0, False)),
            BUSINESS, CONFIG)
        self.assertEqual(state, UNAVAILABLE)

    def test_resource_fatal_is_unavailable(self):
        state, _ = classify_availability(
            video("FAILED", stream(10, 5), stream(10, 5), fatal=True),
            BUSINESS, CONFIG)
        self.assertEqual(state, UNAVAILABLE)

    def test_business_failure_preserves_video_availability(self):
        state, details = classify_availability(
            video("DEGRADED", stream(10, 5), stream(10, 5)), None, CONFIG)
        self.assertEqual(state, OPERATIONAL_DEGRADED)
        self.assertFalse(details["business_healthy"])


if __name__ == "__main__":
    unittest.main()
