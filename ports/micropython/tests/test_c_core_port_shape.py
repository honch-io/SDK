import unittest
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
MICROPYTHON = ROOT / "ports" / "micropython"
USERMOD = MICROPYTHON / "usermod" / "honch"


def canonical_version():
    """The single source of truth: HONCH_SDK_VERSION in the canonical header."""
    text = (ROOT / "core" / "src" / "honch_internal.h").read_text(encoding="utf-8")
    match = re.search(r'#define HONCH_SDK_VERSION "([^"]+)"', text)
    assert match, "HONCH_SDK_VERSION not found in core/src/honch_internal.h"
    return match.group(1)


class MicroPythonCCorePortShapeTests(unittest.TestCase):
    def read(self, relative):
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_user_c_module_declares_core_sources(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")

        self.assertIn("INTERFACE_LIBRARY", cmake)
        self.assertIn("honch_micropython", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_capture_transport.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_core.c", cmake)
        # honch_core.c calls honch_coredump_upload_step_locked; if the coredump
        # source is not listed the unix/esp32 builds fail to link it.
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_coredump.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_event_record.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_packetizer.c", cmake)

    def test_unix_makefile_lists_the_same_core_sources(self):
        # The unix port builds via micropython.mk, not the CMake list — they must
        # stay in sync, or `make -C ports/unix` fails to link (e.g. a missing
        # honch_coredump.c surfaces only on the unix build, not the host tests).
        makefile = self.read("ports/micropython/usermod/honch/micropython.mk")
        for source in ("honch_core.c", "honch_coredump.c", "honch_wire_v2.c"):
            self.assertIn(source, makefile)

    def test_user_c_module_does_not_override_firmware_heap_size(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")
        makefile = self.read("ports/micropython/usermod/honch/micropython.mk")

        self.assertNotIn("MICROPY_C_HEAP_SIZE", cmake)
        self.assertNotIn("MICROPY_C_HEAP_SIZE", makefile)

    def test_c_binding_registers_honch_core_module_and_delegates_to_c_core(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")

        self.assertIn('MP_REGISTER_MODULE(MP_QSTR__honch_core', module)
        self.assertIn("honch_core_init(&self->client", module)
        self.assertIn("honch_core_track(self->client", module)
        self.assertIn("honch_core_identify(self->client", module)
        self.assertIn("honch_core_tick(self->client", module)
        self.assertIn("honch_core_flush(self->client", module)
        self.assertIn("honch_core_shutdown(self->client", module)

    def test_c_binding_avoids_unused_config_helpers(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")

        self.assertNotIn("honch_mp_map_get_bool", module)

    def test_c_binding_iterates_filled_dict_slots(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")

        self.assertIn("i < map->alloc", module)
        self.assertIn("mp_map_slot_is_filled(map, i)", module)
        self.assertNotIn("i < map->used; i++) {\n        mp_map_elem_t *elem = &map->table[i];", module)

    def test_c_binding_rejects_negative_size_values(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")

        self.assertIn("mp_int_t value = mp_obj_get_int(elem->value);", module)
        self.assertIn("if (value <= 0) {", module)
        self.assertIn("mp_raise_ValueError(MP_ERROR_TEXT(\"size config values must be positive\"));", module)
        self.assertNotIn("return (size_t)mp_obj_get_int(elem->value);", module)

    def test_micropython_adapters_implement_core_ops(self):
        adapters = "\n".join(
            [
                self.read("ports/micropython/usermod/honch/mphal_adapter.c"),
                self.read("ports/micropython/usermod/honch/mpstorage_adapter.c"),
                self.read("ports/micropython/usermod/honch/mptransport_adapter.c"),
            ]
        )

        self.assertIn("honch_micropython_platform_ops_init", adapters)
        self.assertIn("honch_micropython_storage_ops_init", adapters)
        self.assertIn("honch_micropython_transport_ops_init", adapters)
        self.assertIn("honch_platform_ops_t", adapters)
        self.assertIn("honch_event_queue_ops_t", adapters)
        self.assertIn("honch_ram_queue_init", adapters)
        self.assertIn("honch_transport_ops_t", adapters)

    def test_micropython_transport_declares_chunk_wire_adapter(self):
        transport = self.read("ports/micropython/usermod/honch/mptransport_adapter.c")
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")
        client = self.read("ports/micropython/honch/client.py")
        config = self.read("ports/micropython/honch/config.py")

        self.assertIn("/capture", transport)
        self.assertIn("honch_mp_post_chunk", transport)
        self.assertIn('"Content-Type", 12), mp_obj_new_str("application/vnd.honch.chunk"', transport)
        self.assertIn('"X-Honch-Project-Key"', transport)
        self.assertIn('"X-Honch-Stream-Id"', transport)
        self.assertIn(".post_chunk = honch_mp_post_chunk", transport)
        self.assertNotIn("MP_QSTR_enable_wire_v2", module)
        self.assertNotIn("enable_wire_v2", config)
        self.assertNotIn("enable_wire_v2", client)

    def test_micropython_transport_passes_configured_timeout_to_transport(self):
        transport = self.read("ports/micropython/usermod/honch/mptransport_adapter.c")
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")
        config = self.read("ports/micropython/honch/config.py")

        # The configured timeout is validated/clamped and threaded into the bounded
        # honch_transport.post_chunk helper (which replaced urequests.post: urequests'
        # settimeout does not bound connect()/handshake on rp2, wedging the VM).
        self.assertIn("#define HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS 10000u", transport)
        self.assertIn("if (timeout_ms == 0u)", transport)
        self.assertIn("return HONCH_STATUS_ERROR_INVALID_ARGUMENT;", transport)
        self.assertIn("if (timeout_ms > HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS)", transport)
        self.assertIn("timeout_ms = HONCH_MP_MAX_TRANSPORT_TIMEOUT_MS;", transport)
        self.assertIn('mp_import_name(qstr_from_str("honch_transport")', transport)
        self.assertIn('mp_load_attr(transport_mod, qstr_from_str("post_chunk"))', transport)
        # honch_transport.post_chunk(url, body, headers, timeout_ms) -> 4 positional args
        self.assertIn("mp_call_function_n_kw(post, 4, 0, args)", transport)
        self.assertIn("transport->timeout_ms", transport)
        self.assertIn("honch_mp_map_get_uint(args[0], MP_QSTR_transport_timeout_ms, DEFAULT_TRANSPORT_TIMEOUT_MS)", module)
        self.assertIn("DEFAULT_TRANSPORT_TIMEOUT_MS = 8000", config)
        self.assertIn("MAX_TRANSPORT_TIMEOUT_MS = 10000", config)

    def test_micropython_exposes_flush_spacing_config(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")
        client = self.read("ports/micropython/honch/client.py")
        config = self.read("ports/micropython/honch/config.py")
        readme = self.read("ports/micropython/README.md")

        self.assertIn("DEFAULT_FLUSH_MIN_INTERVAL_MS = 15000", config)
        self.assertIn("FLUSH_MIN_INTERVAL_DISABLED_MS = 0xFFFFFFFF", config)
        self.assertIn("self.flush_min_interval_ms", config)
        self.assertIn('"flush_min_interval_ms": config.flush_min_interval_ms', client)
        self.assertIn(".flush_min_interval_ms = honch_mp_map_get_uint", module)
        self.assertIn("flush_min_interval_ms", readme)

    def test_micropython_exposes_connectivity_gate(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")
        client = self.read("ports/micropython/honch/client.py")
        config = self.read("ports/micropython/honch/config.py")
        errors = self.read("ports/micropython/honch/errors.py")
        readme = self.read("ports/micropython/README.md")

        self.assertIn("self.connectivity_callback", config)
        self.assertIn("def _connectivity_available", client)
        self.assertIn("OfflineError", client)
        self.assertIn("class OfflineError", errors)
        self.assertIn("ERROR_OFFLINE", module)
        self.assertIn("connectivity_callback", readme)

    def test_python_client_is_thin_wrapper_over_c_module(self):
        client = self.read("ports/micropython/honch/client.py")

        self.assertIn("import _honch_core", client)
        self.assertIn("_honch_core.Client", client)
        self.assertNotIn("PersistentQueue", client)
        self.assertNotIn("IdentityStore", client)
        self.assertNotIn("build_event", client)
        self.assertNotIn("post_batch", client)

    def test_micropython_exposes_runtime_error_reporting(self):
        combined = "\n".join(
            [
                self.read("ports/micropython/honch/client.py"),
                self.read("ports/micropython/usermod/honch/modhonch_core.c"),
                self.read("ports/micropython/README.md"),
            ]
        )

        # Automatic capture: uncaught exceptions -> $crash, logged errors -> $error.
        self.assertIn("def report_log_error", combined)
        self.assertIn("MP_QSTR_report_crash", combined)
        self.assertIn("MP_QSTR_report_log_error", combined)
        self.assertIn("honch_core_report_crash", combined)
        self.assertIn("honch_core_report_log_error", combined)
        self.assertNotIn("honch_core_report_error", combined)
        self.assertNotIn("capture_error", combined)
        # Structured error context: last_error() returns http_status + reason.
        self.assertIn("MP_QSTR_last_error", combined)
        self.assertIn("honch_core_get_last_error", combined)

    def test_package_metadata_versions_match(self):
        init_py = self.read("ports/micropython/honch/__init__.py")
        package_json = json.loads(self.read("ports/micropython/package.json"))
        pyproject = self.read("ports/micropython/pyproject.toml")

        init_match = re.search(r'__version__ = "([^"]+)"', init_py)
        pyproject_match = re.search(r'^version = "([^"]+)"', pyproject, re.MULTILINE)

        self.assertIsNotNone(init_match)
        self.assertIsNotNone(pyproject_match)
        self.assertEqual(canonical_version(), init_match.group(1))
        self.assertEqual(init_match.group(1), package_json["version"])
        self.assertEqual(init_match.group(1), pyproject_match.group(1))

    def test_readme_clarifies_circuitpython_is_not_supported(self):
        readme = self.read("ports/micropython/README.md")

        self.assertIn("This port targets MicroPython", readme)
        self.assertIn("CircuitPython is not covered", readme)
        self.assertIn("_honch_core", readme)

    def test_readme_documents_typed_values_and_volatile_queue(self):
        readme = self.read("ports/micropython/README.md")
        compact_readme = " ".join(readme.split())

        self.assertIn("typed event values", readme)
        self.assertIn("caller-provided", readme)
        self.assertIn("`device_id` is required", readme)
        self.assertIn("volatile by default", readme)
        self.assertIn("compact wire-v2 packetization", readme)
        self.assertIn("`bytes`", readme)
        self.assertIn("Capture may reject bytes unless the project enables binary properties", compact_readme)
        self.assertIn("SDK-owned auto property keys supplied by users are rejected", compact_readme)
        self.assertNotIn("CBOR", readme)
        self.assertNotIn("JSON-compatible", readme)

    def test_pico_w_example_uses_frozen_or_normal_imports(self):
        example = self.read("ports/micropython/examples/pico_w_main.py")

        self.assertIn("from honch import Honch", example)
        self.assertIn("import secrets", example)
        self.assertNotIn('honch.__path__ = "/lib/honch"', example)
        self.assertNotIn("WIFI_PASSWORD = \"", example)
        self.assertNotIn('"$device_id"', example)
        self.assertNotIn('"$sdk_platform"', example)

    def test_auto_properties_spec_matches_core_lifecycle_behavior(self):
        spec = self.read("spec/auto-properties.md")
        compact_spec = " ".join(spec.split())

        self.assertIn("User-supplied properties using SDK-owned keys are rejected", compact_spec)
        self.assertIn("Promoted context properties are encoded once in wire-v2 message context", compact_spec)
        self.assertIn("wire-v2 message context", compact_spec)
        self.assertIn("removed from the event properties before wire encoding", compact_spec)
        self.assertNotIn("user-supplied properties with the same key are overwritten", spec)
        self.assertNotIn("just put them in `properties`", spec)
        self.assertIn("`reset()` clears SDK identity, state, and queued events", spec)
        self.assertIn("Connectivity changes are not auto-detected by the portable core", spec)
        automatic = spec.split("These events are emitted automatically by the SDK:", 1)[1].split(
            "### Reset Reason Values", 1
        )[0]
        table = automatic.split("`reset()` clears SDK identity", 1)[0]
        self.assertNotIn("$device_reset", table)
        self.assertNotIn("$connectivity_change", table)


if __name__ == "__main__":
    unittest.main()
