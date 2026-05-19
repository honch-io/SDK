#!/usr/bin/env python3
"""Static migration checks for the ESP-IDF CBOR wire format."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


class EspIdfCborMigrationTest(unittest.TestCase):
    def test_component_declares_cbor_dependency(self) -> None:
        manifest = read("esp-idf/honch/idf_component.yml")
        cmake = read("esp-idf/honch/CMakeLists.txt")

        self.assertIn("espressif/cbor", manifest)
        self.assertIn("espressif/cjson", manifest)
        self.assertRegex(cmake, r"\bcbor\b")
        self.assertRegex(cmake, r"\bespressif__cjson\b")
        self.assertRegex(cmake, r"\besp_timer\b")

    def test_transport_sends_application_cbor_with_optional_gzip(self) -> None:
        transport = read("esp-idf/honch/src/transport.c")
        transport_header = read("esp-idf/honch/src/transport.h")
        kconfig = read("esp-idf/honch/Kconfig")

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

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read("esp-idf/honch/src/queue.c")
        queue_header = read("esp-idf/honch/src/queue.h")

        self.assertIn("honch_payload_t", queue_header)
        self.assertIn("nvs_set_blob", queue)
        self.assertIn("nvs_get_blob", queue)
        self.assertNotIn("nvs_set_str", queue)
        self.assertNotIn("event_json", queue_header)

    def test_transport_treats_bad_requests_as_non_retryable(self) -> None:
        transport = read("esp-idf/honch/src/transport.c")

        self.assertIn("status == 429", transport)
        self.assertRegex(transport, r"status >= 400 && status < 500")

    def test_lifecycle_detects_existing_wifi_connection(self) -> None:
        lifecycle = read("esp-idf/honch/src/lifecycle.c")

        self.assertIn("esp_wifi_sta_get_ap_info", lifecycle)
        self.assertIn("Initial Wi-Fi connection detected", lifecycle)

    def test_example_syncs_time_and_emits_heartbeat(self) -> None:
        example = read("esp-idf/example/main/app_main.c")

        self.assertIn("#include \"esp_sntp.h\"", example)
        self.assertIn("sync_time", example)
        self.assertLess(example.index("sync_time();"), example.index("honch_init(&config)"))
        self.assertIn('honch_track("heartbeat"', example)

    def test_benchtest_app_exercises_sdk_performance_paths(self) -> None:
        cmake = read("esp-idf/benchtest/CMakeLists.txt")
        kconfig = read("esp-idf/benchtest/main/Kconfig.projbuild")
        bench = read("esp-idf/benchtest/main/app_main.c")

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

    def test_encoder_builds_cbor_epoch_millis_payloads(self) -> None:
        encoder = read("esp-idf/honch/src/encoder.c")
        encoder_header = read("esp-idf/honch/src/encoder.h")

        self.assertIn("#include \"cbor.h\"", encoder)
        self.assertIn("honch_encode_event", encoder_header)
        self.assertIn("honch_payload_t", encoder_header)
        self.assertIn("timestamp", encoder)
        self.assertIn("timestamp_millis", encoder)
        self.assertNotIn("get_iso8601_timestamp", encoder)
        self.assertNotIn("cJSON_AddStringToObject(root, \"timestamp\"", encoder)

    def test_wire_format_docs_describe_cbor_contract(self) -> None:
        spec = read("spec/wire-format.md")
        readme = read("esp-idf/README.md")

        self.assertIn("Content-Type: application/cbor", spec)
        self.assertIn("epoch milliseconds", spec)
        self.assertIn("CBOR", readme)
        self.assertNotIn("Content-Type: application/json", spec)
        self.assertNotIn("ISO-8601 UTC", spec)


if __name__ == "__main__":
    unittest.main(verbosity=2)
