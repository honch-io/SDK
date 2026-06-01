import unittest
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
MICROPYTHON = ROOT / "ports" / "micropython"
USERMOD = MICROPYTHON / "usermod" / "honch"


class MicroPythonCCorePortShapeTests(unittest.TestCase):
    def read(self, relative):
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_user_c_module_declares_core_sources(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")

        self.assertIn("INTERFACE_LIBRARY", cmake)
        self.assertIn("honch_micropython", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_core.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_event_record.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_packetizer.c", cmake)

    def test_rp2_build_reserves_c_heap_for_core_allocations(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")
        makefile = self.read("ports/micropython/usermod/honch/micropython.mk")

        self.assertIn("set(MICROPY_C_HEAP_SIZE 65536)", cmake)
        self.assertIn("MICROPY_C_HEAP_SIZE=65536", cmake)
        self.assertIn("-DMICROPY_C_HEAP_SIZE=65536", makefile)

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

    def test_micropython_transport_passes_configured_timeout_to_requests(self):
        transport = self.read("ports/micropython/usermod/honch/mptransport_adapter.c")

        self.assertIn("honch_mp_timeout_seconds", transport)
        self.assertIn('MP_OBJ_NEW_QSTR(qstr_from_str("timeout"))', transport)
        self.assertIn("mp_call_function_n_kw(post, 1, 3, args)", transport)
        self.assertIn("transport->timeout_ms", transport)

    def test_micropython_exposes_flush_spacing_config(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")
        client = self.read("ports/micropython/honch/client.py")
        config = self.read("ports/micropython/honch/config.py")
        readme = self.read("ports/micropython/README.md")

        self.assertIn("DEFAULT_FLUSH_MIN_INTERVAL_MS = 10000", config)
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

    def test_package_metadata_versions_match(self):
        init_py = self.read("ports/micropython/honch/__init__.py")
        package_json = json.loads(self.read("ports/micropython/package.json"))
        pyproject = self.read("ports/micropython/pyproject.toml")

        init_match = re.search(r'__version__ = "([^"]+)"', init_py)
        pyproject_match = re.search(r'^version = "([^"]+)"', pyproject, re.MULTILINE)

        self.assertIsNotNone(init_match)
        self.assertIsNotNone(pyproject_match)
        self.assertEqual("0.2.0", init_match.group(1))
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
