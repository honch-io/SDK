import json
import os
import re
import unittest

import honch
from honch.errors import (
    CompressionUnavailableError,
    InvalidArgumentError,
    RejectedError,
    ServerError,
)

from .fakes import FakePlatform, FakeTransport


def make_client(tmp_path, **overrides):
    config = {
        "api_key": "test-key",
        "endpoint_url": "http://collector.local/",
        "device_id": "device-1",
        "device_model": "X3-Pro",
        "firmware_version": "3.4.1",
        "environment": "production",
        "queue_directory": str(tmp_path / "honch-state"),
        "batch_size": 2,
        "max_queued_events": 10,
        "max_event_bytes": 8192,
        "transport_timeout_ms": 1000,
        "disable_background_flush": True,
        "platform": FakePlatform(),
        "transport": FakeTransport(),
    }
    config.update(overrides)
    return honch.Honch(**config)


def pending_files(client):
    return sorted(name for name in os.listdir(client.queue.pending_dir) if name.endswith(".json"))


def dead_files(client):
    return sorted(name for name in os.listdir(client.queue.dead_dir) if name.endswith(".json"))


def read_pending_events(client):
    events = []
    for name in pending_files(client):
        with open(os.path.join(client.queue.pending_dir, name), "r", encoding="utf-8") as fh:
            events.append(json.load(fh))
    return events


class HonchSdkTests(unittest.TestCase):
    def test_init_validation_rejects_required_config(self):
        with self.subTest("missing values"):
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                tmp_path = PathShim(tmp)
                base = {
                    "api_key": "key",
                    "endpoint_url": "http://collector.local",
                    "device_model": "model",
                    "firmware_version": "1.0.0",
                    "queue_directory": str(tmp_path),
                }

                for key in list(base):
                    config = dict(base)
                    config[key] = ""
                    with self.assertRaises(InvalidArgumentError):
                        honch.Honch(**config)


class PathShim:
    def __init__(self, value):
        self.value = value

    def __truediv__(self, other):
        return PathShim(os.path.join(self.value, other))

    def __str__(self):
        return self.value


def with_tempdir(fn):
    def wrapper(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            return fn(self, PathShim(tmp))

    return wrapper


class HonchSdkBehaviorTests(unittest.TestCase):
    @with_tempdir
    def test_track_persists_event_with_auto_properties_and_timestamp(self, tmp_path):
        platform = FakePlatform()
        client = make_client(tmp_path, platform=platform)

        client.track(
            "button_pressed",
            {"button": "power", "$device_id": "spoofed", "$sdk_platform": "spoofed"},
        )

        events = read_pending_events(client)
        self.assertEqual([event["event"] for event in events], ["$device_boot", "button_pressed"])
        tracked = events[1]
        self.assertEqual(tracked["distinct_id"], "device-1")
        self.assertRegex(tracked["timestamp"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
        self.assertEqual(tracked["properties"]["button"], "power")
        self.assertEqual(tracked["properties"]["$device_id"], "device-1")
        self.assertEqual(tracked["properties"]["$device_model"], "X3-Pro")
        self.assertEqual(tracked["properties"]["$firmware_version"], "3.4.1")
        self.assertEqual(tracked["properties"]["$sdk_platform"], "micropython")
        self.assertEqual(tracked["properties"]["$sdk_version"], "0.1.0")
        self.assertEqual(tracked["properties"]["$environment"], "production")

    @with_tempdir
    def test_flush_sends_gzip_batch_and_drains_multiple_batches(self, tmp_path):
        transport = FakeTransport([202])
        client = make_client(tmp_path, transport=transport, batch_size=2)
        client.track("first")
        client.track("second")
        client.track("third")

        client.flush()

        self.assertEqual(pending_files(client), [])
        self.assertEqual(len(transport.calls), 2)
        self.assertEqual(transport.calls[0]["url"], "http://collector.local/batch")
        self.assertEqual(transport.calls[0]["headers"]["Content-Type"], "application/json")
        self.assertEqual(transport.calls[0]["headers"]["Content-Encoding"], "gzip")
        self.assertEqual(transport.calls[0]["body"][:2], b"\x1f\x8b")
        self.assertEqual(transport.calls[0]["json"]["token"], "test-key")
        self.assertEqual(len(transport.calls[0]["json"]["batch"]), 2)
        self.assertEqual(transport.calls[1]["json"]["batch"][-1]["event"], "third")

    @with_tempdir
    def test_generated_device_id_and_identify_persist_across_restart(self, tmp_path):
        queue_dir = str(tmp_path / "state")
        platform = FakePlatform()
        client = make_client(tmp_path, queue_directory=queue_dir, device_id=None, platform=platform)
        first_id = client.get_device_id()
        self.assertEqual(len(first_id), 32)

        client.identify("user-1", {"plan": "beta"})
        client.shutdown()

        restarted = make_client(tmp_path, queue_directory=queue_dir, device_id=None, platform=platform)
        self.assertEqual(restarted.get_device_id(), first_id)
        restarted.track("after_restart")
        events = read_pending_events(restarted)
        self.assertEqual(events[-1]["distinct_id"], "user-1")

    @with_tempdir
    def test_sessions_battery_low_firmware_update_and_reset(self, tmp_path):
        queue_dir = str(tmp_path / "state")
        battery = {"level": 55}

        def battery_callback():
            return battery["level"]

        transport = FakeTransport([202])
        client = make_client(
            tmp_path,
            queue_directory=queue_dir,
            firmware_version="1.0.0",
            battery_callback=battery_callback,
            battery_low_threshold=20,
            transport=transport,
        )
        client.flush()
        client.shutdown()

        battery["level"] = 12
        client = make_client(
            tmp_path,
            queue_directory=queue_dir,
            firmware_version="1.1.0",
            battery_callback=battery_callback,
            battery_low_threshold=20,
            transport=transport,
            batch_size=10,
        )
        old_id = client.get_device_id()
        client.session_start("recording")
        client.track("recording_started", {"mode": "hdr"})
        client.session_end()
        client.flush()

        events = [event for call in transport.calls for event in call["json"]["batch"]]
        names = [event["event"] for event in events]
        self.assertIn("$firmware_update", names)
        self.assertIn("$battery_low", names)
        self.assertIn("$session_start", names)
        self.assertIn("$session_end", names)
        session_event = next(event for event in events if event["event"] == "recording_started")
        self.assertTrue(session_event["properties"]["$session_id"].startswith("sess_"))
        low_event = next(event for event in events if event["event"] == "$battery_low")
        self.assertEqual(low_event["properties"]["level"], 12)
        self.assertEqual(low_event["properties"]["$battery_level"], 12)

        client.set_property("legacy_context", "session-1")
        client.reset()
        self.assertNotEqual(client.get_device_id(), old_id)
        self.assertEqual(pending_files(client), [])
        self.assertEqual(dead_files(client), [])
        client.track("after_reset")
        self.assertEqual(read_pending_events(client)[0]["distinct_id"], client.get_device_id())

    @with_tempdir
    def test_validation_queue_limit_retry_rejection_and_compression_errors(self, tmp_path):
        client = make_client(tmp_path, max_queued_events=2)
        with self.assertRaises(InvalidArgumentError):
            client.track("", {})
        with self.assertRaises(InvalidArgumentError):
            client.track("bad", [])
        with self.assertRaises(InvalidArgumentError):
            client.identify("", {})

        client.track("first")
        client.track("second")
        client.track("third")
        self.assertEqual([event["event"] for event in read_pending_events(client)], ["second", "third"])

        retry_transport = FakeTransport([500, 202])
        client.transport = retry_transport
        with self.assertRaises(ServerError):
            client.flush()
        self.assertEqual([event["event"] for event in read_pending_events(client)], ["second", "third"])
        client.flush()
        self.assertEqual(pending_files(client), [])

        rejected = make_client(tmp_path / "reject", transport=FakeTransport([400]), batch_size=10)
        rejected.track("will_reject")
        with self.assertRaises(RejectedError):
            rejected.flush()
        self.assertEqual(pending_files(rejected), [])
        self.assertEqual(len(dead_files(rejected)), 2)

        no_gzip = make_client(
            tmp_path / "nogzip",
            platform=FakePlatform(gzip_enabled=False),
            transport=FakeTransport([202]),
        )
        with self.assertRaises(CompressionUnavailableError):
            no_gzip.flush()
        self.assertEqual(len(pending_files(no_gzip)), 1)
        self.assertEqual(read_pending_events(no_gzip)[0]["event"], "$device_boot")

    @with_tempdir
    def test_background_scheduler_flushes_at_threshold_and_can_be_disabled(self, tmp_path):
        transport = FakeTransport([202])
        client = make_client(
            tmp_path,
            transport=transport,
            disable_background_flush=False,
            flush_event_threshold=2,
            flush_interval_seconds=60,
        )

        client.track("threshold_event")

        self.assertEqual(pending_files(client), [])
        self.assertEqual(len(transport.calls), 1)
        self.assertEqual(
            [event["event"] for event in transport.calls[0]["json"]["batch"]],
            ["$device_boot", "threshold_event"],
        )

        disabled_transport = FakeTransport([202])
        disabled = make_client(
            tmp_path / "disabled",
            transport=disabled_transport,
            disable_background_flush=True,
            flush_event_threshold=2,
        )
        disabled.track("queued")
        self.assertEqual(len(disabled_transport.calls), 0)
        self.assertEqual(len(pending_files(disabled)), 2)

    @with_tempdir
    def test_scheduler_registers_optional_interval_adapter(self, tmp_path):
        platform = FakePlatform()
        transport = FakeTransport([202])
        client = make_client(
            tmp_path,
            platform=platform,
            transport=transport,
            disable_background_flush=False,
            flush_event_threshold=100,
            flush_interval_seconds=5,
        )

        self.assertIsNotNone(platform.periodic)
        self.assertEqual(platform.periodic.interval_ms, 5000)
        client.track("interval_event")
        self.assertEqual(len(transport.calls), 0)

        platform.periodic.fire()

        self.assertEqual(pending_files(client), [])
        self.assertEqual(len(transport.calls), 1)
        self.assertEqual(
            [event["event"] for event in transport.calls[0]["json"]["batch"]],
            ["$device_boot", "interval_event"],
        )
        client.shutdown()
        self.assertTrue(platform.periodic.stopped)


if __name__ == "__main__":
    unittest.main()
