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
        self.assertIn("HONCH_FLUSH_TIMING", component_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../../..")', example_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', bench_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', footprint_cmake)

    def test_esp_transport_ops_post_chunk_wire_to_capture_endpoint(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_transport_http.c"', cmake)
        self.assertIn("#include \"honch/core/transport.h\"", adapter)
        self.assertIn("esp_http_client_init", transport)
        self.assertIn("#include \"esp_event.h\"", transport)
        self.assertIn("#include \"esp_netif.h\"", transport)
        self.assertIn("esp_event_loop_create_default()", transport)
        self.assertIn("esp_netif_init()", transport)
        self.assertIn('"/capture"', transport)
        self.assertIn('"Content-Type", "application/vnd.honch.chunk"', transport)
        self.assertIn('"X-Honch-Project-Key"', transport)
        self.assertIn('"X-Honch-Stream-Id"', transport)
        self.assertIn("HONCH_TRANSPORT_CHUNK_STORED", transport)
        self.assertIn("HONCH_HTTP_TIMING", transport)
        self.assertIn(".post_chunk = honch_esp_post_chunk", transport)
        self.assertIn(".ctx = NULL", transport)
        self.assertNotIn(".ctx = ctx", transport)
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
        self.assertIn("honch_esp_event_queue_ops_init(", compat)
        self.assertIn("config->event_queue_ops", compat)
        self.assertIn("honch_esp_transport_ops_init(&transport_ops", compat)
        self.assertIn("honch_core_init(&next", compat)
        self.assertIn("honch_esp_client_acquire(&client)", compat)
        self.assertIn("honch_core_track(client", compat)
        self.assertIn("honch_core_flush(client)", compat)
        self.assertNotIn("honch_core_track(s_client", compat)
        self.assertNotIn("honch_core_flush(s_client)", compat)
        self.assertIn("honch_client_t", adapter)
        self.assertIn("honch_state_prepare", shims)

    def test_esp_default_storage_is_ram_queue_only(self) -> None:
        storage = read("ports/esp-idf/honch/src/esp_storage.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_storage.c"', cmake)
        self.assertIn('"core/src/honch_ram_queue.c"', cmake)
        self.assertIn("#include \"honch/core/storage.h\"", adapter)
        self.assertIn("#include \"honch/core/ram_queue.h\"", adapter)
        self.assertIn("honch_ram_queue_init", storage)
        self.assertIn("honch_ram_queue_ops_init", storage)
        self.assertNotIn("nvs_", storage.lower())
        self.assertNotIn("nvs_flash", cmake)
        self.assertNotIn("esp_storage_nvs", cmake)

    def test_public_config_has_no_legacy_wire_toggles(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")

        combined = compat + public_header + readme
        self.assertNotIn("enable_wire_v2", combined)
        self.assertNotIn("disable_gzip", combined)
        self.assertNotIn("gzip_min_bytes", combined)

    def test_readme_tracks_typed_properties_not_json_strings(self) -> None:
        readme = read("ports/esp-idf/README.md")

        self.assertIn("honch_property_t", readme)
        self.assertIn("honch_track(\"button_pressed\", button_props, 1u)", readme)
        self.assertNotIn("honch_track(\"button_pressed\", \"{", readme)
        self.assertNotIn("honch_track(\"screen_viewed\", \"{", readme)

    def test_esp_idf_uses_cooperative_tick_config(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")

        self.assertIn("flush_interval_seconds", public_header)
        self.assertIn("flush_event_threshold", public_header)
        self.assertIn("honch_tick", public_header)
        self.assertIn("honch_core_tick(client)", compat)
        self.assertIn("if (config->flush_event_threshold > 0u)", compat)
        self.assertIn(
            "core_config.batch_size = config->flush_event_threshold > HONCH_MAX_BATCH_SIZE",
            compat,
        )
        self.assertIn("core_config.flush_interval_seconds = config->flush_interval_seconds", compat)
        self.assertIn("core_config.flush_event_threshold = config->flush_event_threshold", compat)
        self.assertNotIn("disable_background_flush", compat + public_header + readme)
        self.assertNotIn("flush worker", readme)

    def test_readme_only_documents_implemented_automatic_esp_properties(self) -> None:
        readme = read("ports/esp-idf/README.md")
        automatic = readme.split("## What gets sent automatically", 1)[1].split("## Configuration options", 1)[0]

        self.assertIn("$device_id` — from config or derived from the ESP MAC address", automatic)
        self.assertIn("derived from the ESP MAC address", automatic)
        self.assertNotIn("$wifi_rssi", automatic)
        self.assertIn("$device_boot` — on init, with `reset_reason` property", automatic)
        self.assertNotIn("$connectivity_change", automatic)

    def test_esp_package_metadata_matches_runtime_sdk_version(self) -> None:
        internal = read("core/src/honch_internal.h")
        root_manifest = read("idf_component.yml")
        component_manifest = read("ports/esp-idf/honch/idf_component.yml")
        readme = read("ports/esp-idf/README.md")

        self.assertIn('#define HONCH_SDK_VERSION "0.2.0"', internal)
        self.assertIn('version: "0.2.0"', root_manifest)
        self.assertIn('version: "0.2.0"', component_manifest)
        self.assertIn('idf.py add-dependency "honch-io/honch^0.2.0"', readme)

    def test_esp_component_dependencies_match_port_sources(self) -> None:
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        root_manifest = read("idf_component.yml")
        component_manifest = read("ports/esp-idf/honch/idf_component.yml")

        for dependency in (
            "esp_event",
            "esp_http_client",
            "esp_netif",
            "esp-tls",
            "esp_timer",
            "esp_driver_gpio",
            "driver",
            "freertos",
        ):
            self.assertIn(f"        {dependency}", cmake)

        for unused_dependency in (
            "        esp_wifi",
            "        cbor",
            "        espressif__cjson",
        ):
            self.assertNotIn(unused_dependency, cmake)

        for manifest in (root_manifest, component_manifest):
            self.assertNotIn("espressif/cbor", manifest)
            self.assertNotIn("espressif/cjson", manifest)

    def test_ci_triggers_when_shared_core_changes(self) -> None:
        esp_workflow = read(".github/workflows/esp-idf.yml")
        micropython_workflow = read(".github/workflows/micropython.yml")

        self.assertIn("'core/**'", esp_workflow)
        self.assertIn("'core/**'", micropython_workflow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
