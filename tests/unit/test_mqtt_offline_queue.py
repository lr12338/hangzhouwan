import json
import os
import tempfile
import threading
import time
import unittest

from services.business_enrichment.ais.subscriber import MqttAisSubscriber


class _Store:
    parse_fail = 0


class MqttOfflineQueueTest(unittest.TestCase):
    def test_publish_is_memory_only_and_drops_oldest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "offline.jsonl")
            subscriber = MqttAisSubscriber(
                _Store(), "", event_topic="device/events",
                offline_path=path, offline_max_records=2,
                offline_max_bytes=1024 * 1024)
            subscriber.publish_event({"n": 1})
            subscriber.publish_event({"n": 2})
            subscriber.publish_event({"n": 3})

            self.assertFalse(os.path.exists(path))
            self.assertEqual(subscriber.stats()["offline_queue_records"], 2)
            self.assertEqual(
                [item[0]["n"] for item in subscriber._offline], [2, 3])

            worker = threading.Thread(target=subscriber._event_loop)
            worker.start()
            deadline = time.time() + 2
            while not os.path.exists(path) and time.time() < deadline:
                time.sleep(0.01)
            subscriber._stop = True
            with subscriber._offline_cv:
                subscriber._offline_cv.notify_all()
            worker.join(timeout=2)

            with open(path, encoding="utf-8") as stream:
                persisted = [json.loads(line) for line in stream]
            self.assertEqual([item["n"] for item in persisted], [2, 3])

    def test_v2_disconnect_callback_signature(self):
        subscriber = MqttAisSubscriber(
            _Store(), "", event_topic="device/events")
        subscriber._on_disconnect(None, None, None, 1, None)
        self.assertEqual(subscriber.reconnect_count, 1)


if __name__ == "__main__":
    unittest.main()
