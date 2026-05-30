import importlib
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

    def session_start(self, session_name):
        self.calls.append(("session_start", session_name))

    def session_end(self):
        self.calls.append(("session_end",))

    def connectivity_changed(self, connected):
        self.calls.append(("connectivity_changed", connected))

    def flush(self):
        self.calls.append(("flush",))

    def reset(self):
        self.calls.append(("reset",))

    def shutdown(self):
        self.calls.append(("shutdown",))

    def get_device_id(self):
        return self.device_id


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
            device_model="model",
            firmware_version="1.0",
            queue_directory="/honch",
            batch_size=3,
        )

        self.assertEqual(client.get_device_id(), "device-from-core")
        self.assertEqual(client._core.config["api_key"], "key")
        self.assertEqual(client._core.config["batch_size"], 3)
        self.assertNotIn("enable_wire_v2", client._core.config)

        client.track("pressed", {"button": "power"})
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
            device_model="model",
            firmware_version="1.0",
            queue_directory="/honch",
        )
        client._core.flush = lambda: (_ for _ in ()).throw(RuntimeError("transport error"))

        with self.assertRaises(honch.TransportError):
            client.flush()

    def test_client_does_not_expose_legacy_wire_format_opt_in(self):
        honch = importlib.import_module("honch")

        client = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_model="model",
            firmware_version="1.0",
            queue_directory="/honch",
        )

        self.assertFalse(hasattr(client.config, "enable_wire_v2"))
        self.assertNotIn("enable_wire_v2", client._core.config)


if __name__ == "__main__":
    unittest.main()
