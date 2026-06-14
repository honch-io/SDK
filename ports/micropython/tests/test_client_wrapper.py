import importlib
from pathlib import Path
import sys
import types
import unittest


class FakeCoreError(Exception):
    def __init__(self, status, message="fake core error"):
        super().__init__(message)
        self.status = status


class FakeCoreClient:
    def __init__(self, config):
        self.config = dict(config)
        self.calls = []
        self.device_id = "device-from-core"

    def track(self, event_name, properties):
        self.calls.append(("track", event_name, properties))

    def identify(self, distinct_id, traits):
        self.calls.append(("identify", distinct_id, traits))

    def set_property(self, key, value):
        self.calls.append(("set_property", key, value))

    def report_error(self, report, properties):
        self.calls.append(("report_error", report, properties))

    def session_start(self, session_name):
        self.calls.append(("session_start", session_name))

    def session_end(self):
        self.calls.append(("session_end",))

    def connectivity_changed(self, connected):
        self.calls.append(("connectivity_changed", connected))

    def flush(self):
        self.calls.append(("flush",))

    def tick(self):
        self.calls.append(("tick",))

    def reset(self):
        self.calls.append(("reset",))

    def shutdown(self):
        self.calls.append(("shutdown",))

    def get_device_id(self):
        return self.device_id

    def queue_stats(self):
        return {"depth": 0}


class ClientWrapperTests(unittest.TestCase):
    def setUp(self):
        self.saved = {name: module for name, module in sys.modules.items() if name == "_honch_core" or name == "honch" or name.startswith("honch.")}
        for name in list(sys.modules):
            if name == "_honch_core" or name == "honch" or name.startswith("honch."):
                del sys.modules[name]

        fake = types.ModuleType("_honch_core")
        fake.Client = FakeCoreClient
        fake.Error = FakeCoreError
        fake.ERROR_INVALID_ARGUMENT = 1
        fake.ERROR_IO = 3
        fake.ERROR_TRANSPORT = 4
        fake.ERROR_RATE_LIMITED = 5
        fake.ERROR_SERVER = 6
        fake.ERROR_REJECTED = 7
        fake.ERROR_NOT_INITIALIZED = 8
        fake.ERROR_QUEUE_FULL = 10
        fake.ERROR_TIMEOUT = 11
        fake.ERROR_OFFLINE = 15
        sys.modules["_honch_core"] = fake

    def tearDown(self):
        for name in list(sys.modules):
            if name == "_honch_core" or name == "honch" or name.startswith("honch."):
                del sys.modules[name]
        sys.modules.update(self.saved)

    def test_client_delegates_public_methods_to_c_core_client(self):
        honch = importlib.import_module("honch")

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
            batch_size=3,
        )

        self.assertEqual(client.get_device_id(), "device-from-core")
        self.assertEqual(client.queue_stats(), {"depth": 0})
        self.assertEqual(client._core.config["api_key"], "key")
        self.assertEqual(client._core.config["device_id"], "device-1")
        self.assertEqual(client._core.config["batch_size"], 3)
        self.assertNotIn("enable_wire_v2", client._core.config)

        client.track("pressed", {"button": "power"})
        client.report_error("runtime failed", severity="error", error_type="RuntimeError", properties={"handled": False})
        client.identify("user-1", {"plan": "beta"})
        client.set_property("mode", "hdr")
        client.session_start("recording")
        client.session_end()
        client.connected()
        client.disconnected()
        client.flush()
        client.reset()
        client.shutdown()

        self.assertEqual(
            client._core.calls,
            [
                ("track", "pressed", {"button": "power"}),
                (
                    "report_error",
                    {
                        "message": "runtime failed",
                        "severity": "error",
                        "type": "RuntimeError",
                        "component": None,
                        "code": None,
                        "backtrace": None,
                    },
                    {"handled": False},
                ),
                ("identify", "user-1", {"plan": "beta"}),
                ("set_property", "mode", "hdr"),
                ("session_start", "recording"),
                ("session_end",),
                ("connectivity_changed", True),
                ("connectivity_changed", False),
                ("flush",),
                ("reset",),
                ("shutdown",),
            ],
        )

    def test_maps_runtime_error_message_from_micropython_native_module(self):
        honch = importlib.import_module("honch")

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
        )
        client._core.flush = lambda: (_ for _ in ()).throw(RuntimeError("transport error"))

        with self.assertRaises(honch.TransportError):
            client.flush()

    def test_client_does_not_expose_legacy_wire_format_opt_in(self):
        honch = importlib.import_module("honch")

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
        )

        self.assertFalse(hasattr(client.config, "enable_wire_v2"))
        self.assertNotIn("enable_wire_v2", client._core.config)

    def test_connectivity_callback_skips_tick_and_blocks_flush(self):
        honch = importlib.import_module("honch")
        state = {"connected": False}

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
            connectivity_callback=lambda: state["connected"],
        )

        client.tick()
        with self.assertRaises(honch.OfflineError):
            client.flush()
        self.assertEqual(client._core.calls, [])

    def test_requires_caller_provided_device_id(self):
        honch = importlib.import_module("honch")

        with self.assertRaises(honch.InvalidArgumentError):
            honch.Honch(
                api_key="key",
                endpoint_url="http://collector.local",
                device_model="model",
                firmware_version="1.0",
                event_buffer=bytearray(8192),
            )

    def test_rejects_explicit_zero_or_negative_transport_timeout(self):
        honch = importlib.import_module("honch")

        base_config = {
            "api_key": "key",
            "endpoint_url": "http://collector.local",
            "device_id": "device-1",
            "device_model": "model",
            "firmware_version": "1.0",
            "event_buffer": bytearray(8192),
        }

        client = honch.Honch(**base_config)
        self.assertEqual(client.config.transport_timeout_ms, 3000)

        for timeout_ms in (0, -1):
            config = dict(base_config)
            config["transport_timeout_ms"] = timeout_ms
            with self.assertRaises(honch.InvalidArgumentError):
                honch.Honch(**config)

    def test_rejects_zero_or_negative_size_limits(self):
        honch = importlib.import_module("honch")

        base_config = {
            "api_key": "key",
            "endpoint_url": "http://collector.local",
            "device_id": "device-1",
            "device_model": "model",
            "firmware_version": "1.0",
            "event_buffer": bytearray(8192),
        }

        for key in ("max_queued_events", "max_event_bytes"):
            for value in (0, -1):
                config = dict(base_config)
                config[key] = value
                with self.subTest(key=key, value=value):
                    with self.assertRaises(honch.InvalidArgumentError):
                        honch.Honch(**config)

    def test_connectivity_callback_allows_tick_and_flush_when_online(self):
        honch = importlib.import_module("honch")
        state = {"connected": False}

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
            connectivity_callback=lambda: state["connected"],
        )

        state["connected"] = True
        client.tick()
        client.flush()
        self.assertEqual(client._core.calls, [("tick",), ("flush",)])

    def test_readme_warns_tick_and_flush_hold_the_interpreter(self):
        readme = (Path(__file__).resolve().parents[1] / "README.md").read_text(encoding="utf-8")
        normalized = " ".join(readme.split())

        self.assertIn("client.tick() and client.flush() may block for up to the configured transport timeout", normalized)
        self.assertIn("urequests.post holds the MicroPython interpreter", normalized)
        self.assertIn("Do not call tick() from a latency-sensitive control loop", normalized)
        self.assertIn("Do not call tick() or flush() from ISR-adjacent callbacks", normalized)
        self.assertIn("high-priority tasks", normalized)
        self.assertIn("maximum of 10000 ms", normalized)
        self.assertIn("transport_timeout_ms` (finite positive milliseconds, clamped to 10000)", normalized)


if __name__ == "__main__":
    unittest.main()
