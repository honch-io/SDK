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

    def report_crash(self, report):
        self.calls.append(("report_crash", report))

    def report_log_error(self, component, message):
        self.calls.append(("report_log_error", component, message))

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
        fake.ERROR_OUT_OF_MEMORY = 2
        fake.ERROR_ALREADY_INITIALIZED = 9
        fake.ERROR_NOT_SUPPORTED = 12
        fake.ERROR_INTERNAL = 13
        fake.ERROR_BUSY = 14
        sys.modules["_honch_core"] = fake

    def tearDown(self):
        for name in list(sys.modules):
            if name == "_honch_core" or name == "honch" or name.startswith("honch."):
                del sys.modules[name]
        sys.modules.update(self.saved)

    def test_status_messages_map_to_honch_error_subclasses(self):
        # On real hardware the _honch_core module raises a bare RuntimeError whose
        # message is honch_status_string(status) and which has no .status
        # attribute, so the message path is what actually runs. Every error string
        # honch_status_string() can return must map to a HonchError subclass --
        # never leak as a bare RuntimeError -- and to the right subtype.
        importlib.import_module("honch")
        client_mod = importlib.import_module("honch.client")
        errors = importlib.import_module("honch.errors")

        cases = {
            "invalid argument": errors.InvalidArgumentError,
            "out of memory": errors.HonchError,
            "io error": errors.StorageError,
            "transport error": errors.TransportError,
            "rate limited": errors.RateLimitedError,
            "server error": errors.ServerError,
            "rejected": errors.RejectedError,
            "not initialized": errors.NotInitializedError,
            "already initialized": errors.HonchError,
            "queue full": errors.StorageError,
            "timeout": errors.TransportError,
            "internal error": errors.HonchError,
            "busy": errors.HonchError,
            "not supported": errors.HonchError,
            "offline": errors.OfflineError,
            "unknown": errors.HonchError,
        }
        for message, expected in cases.items():
            with self.assertRaises(errors.HonchError) as ctx:
                client_mod._raise_mapped(RuntimeError(message))
            self.assertIsInstance(ctx.exception, expected, message)

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
        client.report_log_error("runtime failed", component="camera")
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
                ("report_log_error", "camera", "runtime failed"),
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

    def test_endpoint_url_defaults_to_hosted_capture(self):
        honch = importlib.import_module("honch")

        client = honch.Honch(
            api_key="key",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
        )
        self.assertEqual(client.config.endpoint_url, "https://i.honch.io")

        explicit = honch.Honch(
            api_key="key",
            endpoint_url="http://collector.local",
            device_id="device-1",
            device_model="model",
            firmware_version="1.0",
            event_buffer=bytearray(8192),
        )
        self.assertEqual(explicit.config.endpoint_url, "http://collector.local")

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
        self.assertEqual(client.config.transport_timeout_ms, 8000)

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


    def _make_client(self, honch, **overrides):
        config = {
            "api_key": "key",
            "endpoint_url": "http://collector.local",
            "device_id": "device-1",
            "device_model": "model",
            "firmware_version": "1.0",
            "event_buffer": bytearray(8192),
        }
        config.update(overrides)
        return honch.Honch(**config)

    def test_install_error_hook_is_idempotent_and_reports_once(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        import sys

        saved = sys.excepthook
        original_calls = []
        sys.excepthook = lambda *a: original_calls.append("original")
        try:
            self.assertTrue(client.install_error_hook())
            # A second install must be a no-op, not a second wrapping layer
            # (which would report the same exception twice).
            self.assertTrue(client.install_error_hook())

            try:
                raise ValueError("boom")
            except ValueError as exc:
                sys.excepthook(type(exc), exc, None)

            reports = [c for c in client._core.calls if c[0] == "report_crash"]
            self.assertEqual(len(reports), 1)
            self.assertEqual(reports[0][1]["type"], "ValueError")
            self.assertEqual(original_calls, ["original"])
        finally:
            sys.excepthook = saved

    def test_crash_backtrace_is_compact_and_crash_site_first(self):
        # The $crash backtrace field is small and the core drops it wholesale if
        # over-long, so the formatter must emit the crash site first (and within
        # the byte budget). print_exception is MicroPython-only, so fake it with
        # MicroPython's exact output format.
        importlib.import_module("honch")
        from honch.client import _format_crash_backtrace

        import sys

        def fake_print_exception(exc, buf):
            buf.write(
                "Traceback (most recent call last):\n"
                '  File "app.py", line 88, in <module>\n'
                '  File "app.py", line 10, in loop\n'
                '  File "sensor.py", line 42, in read\n'
                "ValueError: bus timeout\n"
            )

        saved = getattr(sys, "print_exception", None)
        sys.print_exception = fake_print_exception
        try:
            bt = _format_crash_backtrace(ValueError("bus timeout"))
        finally:
            if saved is None:
                del sys.print_exception
            else:
                sys.print_exception = saved

        # Crash site (sensor.py:42) first; outermost frame (<module>) last.
        self.assertEqual(
            bt, "sensor.py:42 in read <- app.py:10 in loop <- app.py:88 in <module>"
        )

    def test_crash_backtrace_returns_none_when_unavailable(self):
        # Must never raise inside the excepthook: if print_exception is missing
        # (or fails) the formatter degrades to None rather than displacing the crash.
        importlib.import_module("honch")
        from honch.client import _format_crash_backtrace

        import sys

        saved = getattr(sys, "print_exception", None)
        if hasattr(sys, "print_exception"):
            del sys.print_exception
        try:
            self.assertIsNone(_format_crash_backtrace(ValueError("boom")))
        finally:
            if saved is not None:
                sys.print_exception = saved

    def test_crash_backtrace_byte_bounded_keeps_crash_site_first(self):
        # Regression: the C core's $crash `backtrace` is an ALL-OR-NOTHING field —
        # honch_append_crash_string drops the WHOLE property when the value is
        # longer than HONCH_FAULT_BACKTRACE_MAX_BYTES (192); it does not truncate.
        # So the formatter must keep its output within that byte budget, dropping
        # the OUTERMOST frames so the crash site survives — otherwise a realistic
        # multi-frame trace ships with no backtrace at all.
        importlib.import_module("honch")
        from honch.client import (
            _format_crash_backtrace,
            _CRASH_BACKTRACE_MAX_BYTES,
        )

        import sys

        # outermost -> innermost (MicroPython prints most-recent-call-LAST); the
        # crash-site-first join of these 10 frames is ~384 bytes, well over 192.
        frames_outer_to_inner = [
            ("main_application.py", 880, "<module>"),
            ("service_controller.py", 712, "start"),
            ("event_dispatch_loop.py", 655, "dispatch"),
            ("hardware_manager.py", 538, "poll_all"),
            ("peripheral_registry.py", 421, "read_each"),
            ("i2c_bus_driver.py", 364, "transfer"),
            ("temperature_sensor.py", 247, "sample"),
            ("register_protocol.py", 190, "read_word"),
            ("low_level_io.py", 133, "read_register"),
            ("fault_inject.py", 42, "boom"),
        ]
        lines = ["Traceback (most recent call last):"]
        for fname, lineno, func in frames_outer_to_inner:
            lines.append('  File "%s", line %d, in %s' % (fname, lineno, func))
        lines.append("ValueError: bus timeout")
        payload = "\n".join(lines) + "\n"

        def fake_print_exception(exc, buf):
            buf.write(payload)

        saved = getattr(sys, "print_exception", None)
        sys.print_exception = fake_print_exception
        try:
            bt = _format_crash_backtrace(ValueError("bus timeout"))
        finally:
            if saved is None:
                del sys.print_exception
            else:
                sys.print_exception = saved

        self.assertIsNotNone(bt)
        # Fits the core's byte budget, so the property is emitted, not dropped.
        self.assertLessEqual(len(bt.encode("utf-8")), _CRASH_BACKTRACE_MAX_BYTES)
        # Crash site (innermost frame) is preserved and comes first.
        self.assertTrue(bt.startswith("fault_inject.py:42 in boom"))
        # The outermost frame is the one dropped to fit — never the crash site.
        self.assertNotIn("main_application.py:880 in <module>", bt)
        # Surviving frames are intact (no mid-frame garbage from truncation).
        for frame in bt.split(" <- "):
            self.assertIn(" in ", frame)

    def test_run_returns_result_and_reports_nothing_on_success(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)
        self.assertEqual(client.run(lambda: 42), 42)
        self.assertEqual([c for c in client._core.calls if c[0] == "report_crash"], [])

    def test_run_reports_crash_with_traceback_and_reraises(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        import sys

        def fake_print_exception(exc, buf):
            buf.write(
                "Traceback (most recent call last):\n"
                '  File "main.py", line 5, in main\n'
                "ValueError: boom\n"
            )

        def boom():
            raise ValueError("boom")

        saved = getattr(sys, "print_exception", None)
        sys.print_exception = fake_print_exception
        try:
            with self.assertRaises(ValueError):
                client.run(boom)
        finally:
            if saved is None:
                del sys.print_exception
            else:
                sys.print_exception = saved

        reports = [c for c in client._core.calls if c[0] == "report_crash"]
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0][1]["type"], "ValueError")
        self.assertIn("main.py:5 in main", reports[0][1].get("backtrace", ""))
        # A fatal crash must be pushed out before the program exits.
        self.assertIn(("flush",), client._core.calls)

    def test_run_captures_crash_without_sys_excepthook(self):
        # The whole point on stock MicroPython (e.g. Pico W): run() does not depend
        # on sys.excepthook, so it captures the crash even when it is absent.
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        import sys

        saved = getattr(sys, "excepthook", None)
        if hasattr(sys, "excepthook"):
            del sys.excepthook

        def boom():
            raise RuntimeError("no hook needed")

        try:
            with self.assertRaises(RuntimeError):
                client.run(boom)
        finally:
            if saved is not None:
                sys.excepthook = saved

        self.assertTrue(any(c[0] == "report_crash" for c in client._core.calls))

    def test_uninstall_error_hook_restores_previous(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        import sys

        saved = sys.excepthook
        sentinel = lambda *a: None
        sys.excepthook = sentinel
        try:
            self.assertTrue(client.install_error_hook())
            self.assertIsNot(sys.excepthook, sentinel)
            self.assertTrue(client.uninstall_error_hook())
            self.assertIs(sys.excepthook, sentinel)
            # Uninstalling when nothing is installed is a no-op.
            self.assertFalse(client.uninstall_error_hook())
        finally:
            sys.excepthook = saved

    def test_excepthook_swallows_report_failure_and_still_chains(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        import sys

        saved = sys.excepthook
        original_calls = []
        sys.excepthook = lambda *a: original_calls.append("original")
        try:
            self.assertTrue(client.install_error_hook())
            client._core.report_crash = lambda *a, **k: (_ for _ in ()).throw(RuntimeError("io error"))

            try:
                raise ValueError("boom")
            except ValueError as exc:
                # The excepthook must never raise, and must still deliver the
                # original exception to the previous hook even if reporting fails.
                sys.excepthook(type(exc), exc, None)

            self.assertEqual(original_calls, ["original"])
        finally:
            sys.excepthook = saved

    def test_report_log_error_forwards_component_and_message(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        client.report_log_error("disk full", component="storage")

        self.assertEqual(
            client._core.calls[0],
            ("report_log_error", "storage", "disk full"),
        )

    def test_validation_rules_for_text_fields(self):
        honch = importlib.import_module("honch")
        client = self._make_client(honch)

        with self.assertRaises(honch.InvalidArgumentError):
            client.track("   ")
        with self.assertRaises(honch.InvalidArgumentError):
            client.track("e" * 129)
        with self.assertRaises(honch.InvalidArgumentError):
            client.identify("  ")
        with self.assertRaises(honch.InvalidArgumentError):
            client.identify("d" * 257)
        with self.assertRaises(honch.InvalidArgumentError):
            client.set_property("  ", "x")
        with self.assertRaises(honch.InvalidArgumentError):
            client.report_log_error("  ")


if __name__ == "__main__":
    unittest.main()
