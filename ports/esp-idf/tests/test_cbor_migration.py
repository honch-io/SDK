#!/usr/bin/env python3
"""Static migration checks for the ESP-IDF CBOR wire format."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


class EspIdfCborMigrationTest(unittest.TestCase):
    def test_esp_platform_ops_wrap_idf_primitives_for_core(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_platform.c"', cmake)
        self.assertIn('"../../../core/include"', cmake)
        self.assertIn("#include \"honch/core/platform.h\"", adapter)
        self.assertIn("esp_timer_get_time() / 1000", platform)
        self.assertIn("esp_fill_random(buffer, buffer_size)", platform)
        self.assertIn("xSemaphoreCreateMutex", platform)
        self.assertIn("xSemaphoreTake", platform)
        self.assertIn("xSemaphoreGive", platform)
        self.assertIn(".lock = honch_esp_lock", platform)
        self.assertIn(".unlock = honch_esp_unlock", platform)
        self.assertIn(".log = honch_esp_log", platform)

    def test_esp_storage_ops_use_nvs_peek_confirm_contract(self) -> None:
        storage = read("ports/esp-idf/honch/src/esp_storage_nvs.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_storage_nvs.c"', cmake)
        self.assertIn("#include \"honch/core/storage.h\"", adapter)
        self.assertIn('HONCH_ESP_STATE_NAMESPACE "honch_state"', storage)
        self.assertIn('HONCH_ESP_QUEUE_NAMESPACE "honch_q"', storage)
        self.assertIn('HONCH_ESP_DEAD_NAMESPACE "honch_dead"', storage)
        self.assertIn("HONCH_ESP_NVS_KEY_SIZE 16", storage)
        self.assertIn('strcmp(key, "firmware_version") == 0', storage)
        self.assertIn('return "fw_ver"', storage)
        self.assertIn("honch_esp_sequence_key", storage)
        self.assertIn("nvs_set_blob(handle, key, event, event_size)", storage)
        self.assertIn("nvs_get_blob(handle, key, NULL, &value_size)", storage)
        self.assertIn("honch_esp_queue_peek", storage)
        self.assertIn("honch_esp_queue_consume", storage)
        self.assertIn("honch_esp_queue_dead_letter", storage)
        self.assertIn("honch_esp_queue_drop_oldest", storage)
        self.assertIn(".queue_peek = honch_esp_queue_peek", storage)
        self.assertIn(".queue_consume = honch_esp_queue_consume", storage)
        self.assertIn(".queue_dead_letter = honch_esp_queue_dead_letter", storage)

    def test_esp_transport_ops_wrap_http_client_for_core(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_transport_http.c"', cmake)
        self.assertIn("#include \"honch/core/transport.h\"", adapter)
        self.assertNotIn("#include \"transport.h\"", transport)
        self.assertIn("esp_http_client_init", transport)
        self.assertIn('"Content-Type", "application/cbor"', transport)
        self.assertIn('"Content-Encoding", "gzip"', transport)
        self.assertIn("tdefl_compress_mem_to_heap", transport)
        self.assertIn("status >= 200 && status < 300", transport)
        self.assertIn("status == 401", transport)
        self.assertIn("status == 429", transport)
        self.assertIn("status >= 400 && status < 500", transport)
        self.assertIn("HONCH_TRANSPORT_ACCEPTED", transport)
        self.assertIn("HONCH_TRANSPORT_AUTH_ERROR", transport)
        self.assertIn("HONCH_TRANSPORT_REJECTED", transport)
        self.assertIn("HONCH_TRANSPORT_RETRY", transport)
        self.assertIn("HONCH_STATUS_ERROR_RATE_LIMITED", transport)
        self.assertIn("HONCH_STATUS_ERROR_SERVER", transport)
        self.assertIn(".post_batch = honch_esp_post_batch", transport)

    def test_esp_compat_layer_delegates_public_api_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_compat.c"', cmake)
        self.assertNotIn('"src/honch.c"', cmake)
        self.assertIn("#define HONCH_CORE_NO_SHORT_STATUS_NAMES", compat)
        self.assertIn("#include \"honch/core/honch.h\"", compat)
        self.assertIn("static honch_client_t *s_client", compat)
        self.assertIn("honch_esp_status_to_err", compat)
        self.assertIn("honch_esp_platform_ops_init(&platform_ops", compat)
        self.assertIn("honch_esp_storage_ops_init(&storage_ops", compat)
        self.assertIn("honch_esp_transport_ops_init(&transport_ops", compat)
        self.assertIn("core_config.disable_background_flush = 1", compat)
        self.assertIn("honch_core_init(&next", compat)
        self.assertIn("honch_core_track(s_client", compat)
        self.assertIn("honch_core_identify(s_client", compat)
        self.assertIn("honch_core_set_property(s_client", compat)
        self.assertIn("honch_core_session_start(s_client", compat)
        self.assertIn("honch_core_session_end(s_client", compat)
        self.assertIn("honch_core_flush(s_client)", compat)
        self.assertIn("honch_core_reset(s_client)", compat)
        self.assertIn("honch_core_shutdown(s_client)", compat)
        self.assertIn("honch_core_get_device_id(s_client)", compat)
        self.assertIn("honch_track_gpio", compat)
        self.assertIn("honch_esp_platform_ops_deinit", compat)
        self.assertIn("honch_client_t", adapter)

    def test_component_declares_cbor_dependency(self) -> None:
        manifest = read("ports/esp-idf/honch/idf_component.yml")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn("espressif/cbor", manifest)
        self.assertIn("espressif/cjson", manifest)
        self.assertRegex(cmake, r"\bcbor\b")
        self.assertRegex(cmake, r"\bespressif__cjson\b")
        self.assertRegex(cmake, r"\besp_timer\b")

    def test_transport_sends_application_cbor_with_optional_gzip(self) -> None:
        transport = read("ports/esp-idf/honch/src/transport.c")
        transport_header = read("ports/esp-idf/honch/src/transport.h")
        kconfig = read("ports/esp-idf/honch/Kconfig")

        self.assertIn('"Content-Type", "application/cbor"', transport)
        self.assertNotIn('"Content-Type", "application/json"', transport)
        self.assertIn('"Content-Encoding", "gzip"', transport)
        self.assertIn("CONFIG_HONCH_ENABLE_GZIP", transport)
        self.assertIn("CONFIG_HONCH_GZIP_MIN_BYTES", transport)
        self.assertIn("miniz.h", transport)
        self.assertIn("tdefl_compress_mem_to_heap", transport)
        self.assertNotIn("mz_deflate", transport)
        self.assertIn("compressed_size < body_len", transport)
        self.assertIn("falls back to raw CBOR", transport_header)
        self.assertIn("HONCH_ENABLE_GZIP", kconfig)
        self.assertIn("depends on !IDF_TARGET_ESP32 && !IDF_TARGET_ESP32S2", kconfig)
        self.assertIn("HONCH_GZIP_MIN_BYTES", kconfig)

    def test_perf_logging_instruments_sdk_hot_paths(self) -> None:
        kconfig = read("ports/esp-idf/honch/Kconfig")
        perf = read("ports/esp-idf/honch/src/perf.h")
        honch = read("ports/esp-idf/honch/src/honch.c")
        encoder = read("ports/esp-idf/honch/src/encoder.c")
        queue = read("ports/esp-idf/honch/src/queue.c")
        scheduler = read("ports/esp-idf/honch/src/scheduler.c")
        transport = read("ports/esp-idf/honch/src/transport.c")
        bench_defaults = read("ports/esp-idf/benchtest/sdkconfig.defaults")

        self.assertIn("HONCH_PERF_LOGGING", kconfig)
        self.assertIn("HONCH_PERF_LOG", perf)
        self.assertIn("CONFIG_HONCH_PERF_LOGGING", perf)
        for marker in (
            "HONCH_PERF_TRACK",
            "HONCH_PERF_ENCODE_EVENT",
            "HONCH_PERF_QUEUE_PUSH",
            "HONCH_PERF_QUEUE_POP",
            "HONCH_PERF_QUEUE_CONFIRM",
            "HONCH_PERF_ENCODE_BATCH",
            "HONCH_PERF_TRANSPORT",
            "HONCH_PERF_FLUSH",
        ):
            self.assertTrue(
                any(marker in text for text in (honch, encoder, queue, scheduler, transport)),
                f"{marker} missing from SDK instrumentation",
            )
        self.assertIn("CONFIG_HONCH_PERF_LOGGING=y", bench_defaults)

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read("ports/esp-idf/honch/src/queue.c")
        queue_header = read("ports/esp-idf/honch/src/queue.h")

        self.assertIn("honch_payload_t", queue_header)
        self.assertIn("nvs_set_blob", queue)
        self.assertIn("nvs_get_blob", queue)
        self.assertNotIn("nvs_set_str", queue)
        self.assertNotIn("event_json", queue_header)

    def test_queue_uses_ram_first_with_nvs_spill(self) -> None:
        queue = read("ports/esp-idf/honch/src/queue.c")
        kconfig = read("ports/esp-idf/honch/Kconfig")

        self.assertIn("HONCH_RAM_QUEUE_DEPTH", kconfig)
        self.assertIn("HONCH_DURABLE_IMMEDIATE_NVS", kconfig)
        self.assertIn("s_ram_events", queue)
        self.assertIn("ram_enqueue", queue)
        self.assertIn("nvs_append_payload", queue)
        self.assertIn("HONCH_PERF_RAM_QUEUE_PUSH", queue)
        self.assertIn("HONCH_PERF_NVS_SPILL", queue)
        self.assertLess(
            queue.index("err = ram_enqueue(payload);"),
            queue.index('err = nvs_append_payload(payload->data, payload->len, "ram_full");'),
        )

    def test_encoder_directly_encodes_properties_without_cjson_merge(self) -> None:
        encoder = read("ports/esp-idf/honch/src/encoder.c")
        kconfig = read("ports/esp-idf/honch/Kconfig")

        self.assertIn("HONCH_WIFI_RSSI_CACHE_MS", kconfig)
        self.assertIn("honch_event_runtime_t", encoder)
        self.assertIn("encode_properties_map", encoder)
        self.assertIn("encode_user_properties_without_reserved_keys", encoder)
        self.assertIn("HONCH_RESERVED_PROPERTY_COUNT", encoder)
        self.assertNotIn("cJSON_Duplicate", encoder)
        self.assertNotIn("cJSON_AddStringToObject", encoder)
        self.assertNotIn("cJSON_AddNumberToObject", encoder)
        self.assertNotIn("cJSON_DeleteItemFromObject", encoder)

    def test_transport_treats_bad_requests_as_non_retryable(self) -> None:
        transport = read("ports/esp-idf/honch/src/transport.c")

        self.assertIn("status == 429", transport)
        self.assertRegex(transport, r"status >= 400 && status < 500")

    def test_lifecycle_detects_existing_wifi_connection(self) -> None:
        lifecycle = read("ports/esp-idf/honch/src/lifecycle.c")

        self.assertIn("esp_wifi_sta_get_ap_info", lifecycle)
        self.assertIn("Initial Wi-Fi connection detected", lifecycle)

    def test_example_syncs_time_and_emits_heartbeat(self) -> None:
        example = read("ports/esp-idf/example/main/app_main.c")

        self.assertIn("#include \"esp_sntp.h\"", example)
        self.assertIn("sync_time", example)
        self.assertLess(example.index("sync_time();"), example.index("honch_init(&config)"))
        self.assertIn('honch_track("heartbeat"', example)

    def test_benchtest_app_exercises_sdk_performance_paths(self) -> None:
        cmake = read("ports/esp-idf/benchtest/CMakeLists.txt")
        kconfig = read("ports/esp-idf/benchtest/main/Kconfig.projbuild")
        bench = read("ports/esp-idf/benchtest/main/app_main.c")

        self.assertIn('project(honch_benchtest)', cmake)
        self.assertIn('config BENCH_EVENT_COUNT', kconfig)
        self.assertIn('config BENCH_FLUSH_EVERY', kconfig)
        for marker in (
            'BENCH_RUN_START',
            'BENCH_TRACK_SUMMARY',
            'BENCH_FLUSH_SUMMARY',
            'BENCH_RESOURCE_SUMMARY',
            'BENCH_RUN_END',
        ):
            self.assertIn(marker, bench)
        self.assertIn("#include \"esp_sntp.h\"", bench)
        self.assertIn("esp_wifi_sta_get_ap_info", bench)
        self.assertIn("uxTaskGetStackHighWaterMark", bench)
        self.assertIn('honch_track("bench_event"', bench)
        self.assertIn("honch_flush()", bench)
        self.assertIn("queued_estimate", bench)

    def test_esp_footprint_report_lives_with_benchmark_results(self) -> None:
        report = read("ports/esp-idf/benchtest/results/esp32-build-footprint-report.json")
        gitignore = read(".gitignore")

        self.assertIn('"target": "esp32"', report)
        self.assertIn('"direct_honch_archive"', report)
        self.assertIn('"runtime"', report)
        self.assertIn('"landing_claims"', report)
        self.assertIn("ports/esp-idf/footprint/", gitignore)

    def test_encoder_builds_cbor_epoch_millis_payloads(self) -> None:
        encoder = read("ports/esp-idf/honch/src/encoder.c")
        encoder_header = read("ports/esp-idf/honch/src/encoder.h")

        self.assertIn("#include \"cbor.h\"", encoder)
        self.assertIn("honch_encode_event", encoder_header)
        self.assertIn("honch_payload_t", encoder_header)
        self.assertIn("timestamp", encoder)
        self.assertIn("timestamp_millis", encoder)
        self.assertNotIn("get_iso8601_timestamp", encoder)
        self.assertNotIn("cJSON_AddStringToObject(root, \"timestamp\"", encoder)

    def test_wire_format_docs_describe_cbor_contract(self) -> None:
        spec = read("spec/wire-format.md")
        readme = read("ports/esp-idf/README.md")

        self.assertIn("Content-Type: application/cbor", spec)
        self.assertIn("epoch milliseconds", spec)
        self.assertIn("CBOR", readme)
        self.assertNotIn("Content-Type: application/json", spec)
        self.assertNotIn("ISO-8601 UTC", spec)


if __name__ == "__main__":
    unittest.main(verbosity=2)
