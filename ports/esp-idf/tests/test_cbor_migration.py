#!/usr/bin/env python3
"""Static checks for the ESP-IDF default chunk wire transport."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


class EspIdfChunkWireTest(unittest.TestCase):
    def test_esp_idf_component_uses_repo_root_package_layout(self) -> None:
        root_cmake = read("CMakeLists.txt")
        root_manifest = read("idf_component.yml")
        component_cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        example_cmake = read("ports/esp-idf/example/CMakeLists.txt")
        bench_cmake = read("ports/esp-idf/benchtest/CMakeLists.txt")
        footprint_cmake = read("ports/esp-idf/footprint/CMakeLists.txt")

        self.assertIn("if(ESP_PLATFORM)", root_cmake)
        self.assertIn("ports/esp-idf/honch/CMakeLists.txt", root_cmake)
        self.assertIn("description: \"Honch product analytics SDK for ESP-IDF\"", root_manifest)
        self.assertIn("HONCH_SDK_ROOT", component_cmake)
        self.assertIn("core/src/honch_core.c", component_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../../..")', example_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', bench_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../../..")', footprint_cmake)

    def test_esp_transport_ops_post_chunk_wire_to_capture_endpoint(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_transport_http.c"', cmake)
        self.assertIn("#include \"honch/core/transport.h\"", adapter)
        self.assertIn("esp_http_client_init", transport)
        self.assertIn('"/capture"', transport)
        self.assertIn('"Content-Type", "application/vnd.honch.chunk"', transport)
        self.assertIn('"X-Honch-Project-Key"', transport)
        self.assertIn('"X-Honch-Stream-Id"', transport)
        self.assertIn("HONCH_TRANSPORT_CHUNK_STORED", transport)
        self.assertIn(".post_chunk = honch_esp_post_chunk", transport)
        self.assertNotIn("post_batch", transport)
        self.assertNotIn("application/cbor", transport)
        self.assertNotIn("Content-Encoding", transport)
        self.assertNotIn("tdefl_compress_mem_to_heap", transport)

    def test_esp_compat_layer_delegates_public_api_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        shims = read("ports/esp-idf/honch/src/esp_core_shims.c")

        self.assertIn('"src/esp_compat.c"', cmake)
        self.assertIn('"src/esp_core_shims.c"', cmake)
        self.assertIn("#define HONCH_CORE_NO_SHORT_STATUS_NAMES", compat)
        self.assertIn("#include \"honch/core/honch.h\"", compat)
        self.assertIn("static honch_client_t *s_client", compat)
        self.assertIn("honch_esp_platform_ops_init(&platform_ops", compat)
        self.assertIn("honch_esp_storage_ops_init(&storage_ops", compat)
        self.assertIn("honch_esp_transport_ops_init(&transport_ops", compat)
        self.assertIn("honch_core_init(&next", compat)
        self.assertIn("honch_core_track(s_client", compat)
        self.assertIn("honch_core_flush(s_client)", compat)
        self.assertIn("honch_client_t", adapter)
        self.assertIn("honch_state_prepare", shims)

    def test_esp_storage_ops_use_nvs_peek_confirm_contract(self) -> None:
        storage = read("ports/esp-idf/honch/src/esp_storage_nvs.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_storage_nvs.c"', cmake)
        self.assertIn("#include \"honch/core/storage.h\"", adapter)
        self.assertIn('HONCH_ESP_STATE_NAMESPACE "honch_state"', storage)
        self.assertIn('HONCH_ESP_QUEUE_NAMESPACE "honch_q"', storage)
        self.assertIn("honch_esp_queue_peek", storage)
        self.assertIn("honch_esp_queue_consume", storage)
        self.assertIn("honch_esp_queue_dead_letter", storage)
        self.assertIn(".queue_peek = honch_esp_queue_peek", storage)
        self.assertIn(".queue_consume = honch_esp_queue_consume", storage)
        self.assertIn(".queue_dead_letter = honch_esp_queue_dead_letter", storage)

    def test_public_config_has_no_legacy_wire_toggles(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")

        combined = compat + public_header + readme
        self.assertNotIn("enable_wire_v2", combined)
        self.assertNotIn("disable_gzip", combined)
        self.assertNotIn("gzip_min_bytes", combined)

    def test_esp_idf_background_flush_config_is_honored(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")

        self.assertIn("flush_interval_seconds", public_header)
        self.assertIn("flush_event_threshold", public_header)
        self.assertIn("core_config.flush_interval_seconds = config->flush_interval_seconds", compat)
        self.assertIn("core_config.flush_event_threshold = config->flush_event_threshold", compat)
        self.assertIn("core_config.disable_background_flush = 0", compat)

    def test_ci_triggers_when_shared_core_changes(self) -> None:
        esp_workflow = read(".github/workflows/esp-idf.yml")
        micropython_workflow = read(".github/workflows/micropython.yml")

        self.assertIn("'core/**'", esp_workflow)
        self.assertIn("'core/**'", micropython_workflow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
