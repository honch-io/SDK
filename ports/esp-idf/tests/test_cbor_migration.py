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
        self.assertIn("gettimeofday(&now", platform)
        self.assertIn("HONCH_MIN_UNIX_TIME_SECONDS = 1577836800", platform)
        self.assertIn("now.tv_sec >= HONCH_MIN_UNIX_TIME_SECONDS", platform)
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

    def test_esp_storage_queue_is_ram_first_with_nvs_fallback(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        storage = read("ports/esp-idf/honch/src/esp_storage_nvs.c")

        self.assertIn("honch_esp_storage_use_ram_queue", adapter)
        self.assertIn("honch_esp_storage_use_ram_queue", compat)
        self.assertIn("&s_storage_ctx", compat)
        self.assertIn("config->event_buffer", compat)
        self.assertIn("config->event_buffer_size", compat)

        self.assertIn("honch_esp_ram_queue_push", storage)
        self.assertIn("honch_esp_ram_queue_peek", storage)
        self.assertIn("honch_esp_ram_queue_consume", storage)
        self.assertIn("honch_esp_nvs_queue_push", storage)
        self.assertIn("honch_esp_nvs_queue_peek", storage)
        self.assertIn("honch_esp_nvs_queue_consume", storage)

        self.assertIn("honch_esp_ram_queue_push(storage, event, event_size, sequence)", storage)
        self.assertIn("return honch_esp_nvs_queue_push(ctx, event, event_size, sequence)", storage)
        self.assertIn("honch_esp_ram_queue_has_events(storage)", storage)
        self.assertIn("return honch_esp_nvs_queue_peek(ctx, reader)", storage)

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
        shims = read("ports/esp-idf/honch/src/esp_core_shims.c")

        self.assertIn('"src/esp_compat.c"', cmake)
        self.assertIn('"src/esp_core_shims.c"', cmake)
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
        self.assertNotIn("../../../c-core/honch/include", cmake)
        self.assertIn("honch_state_prepare", shims)
        self.assertIn("honch_state_check_firmware_version", shims)
        self.assertIn("honch_random_hex", shims)
        self.assertIn("esp_fill_random(bytes, sizeof(bytes))", shims)
        self.assertIn("client->storage->state_get", shims)
        self.assertIn("client->storage->queue_push", shims)
        self.assertIn("client->storage->queue_clear", shims)

    def test_esp_compat_rejects_truncated_static_config_copies(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        self.assertIn("honch_esp_copy_static_config_string", compat)
        self.assertIn("return HONCH_ERR_INVALID_ARG", compat)
        self.assertIn('honch_esp_copy_static_config_string(s_api_key, sizeof(s_api_key), config->api_key)', compat)
        self.assertIn('honch_esp_copy_static_config_string(s_device_model, sizeof(s_device_model), config->device_model)', compat)
        self.assertIn("s_firmware_version", compat)
        self.assertIn("sizeof(s_firmware_version)", compat)
        self.assertIn("config->firmware_version", compat)
        self.assertIn('honch_esp_copy_static_config_string(s_environment, sizeof(s_environment), environment)', compat)

    def test_esp_gpio_adapter_preserves_gpio_tracking_worker(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        gpio = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_gpio_adapter.c"', cmake)
        self.assertNotIn('"src/gpio.c"', cmake)
        self.assertIn('#include "esp_gpio_adapter.h"', compat)
        self.assertIn("honch_gpio_init()", compat)
        self.assertIn("honch_gpio_deinit()", compat)
        self.assertIn("honch_gpio_register(pin, event_name, mode)", compat)
        self.assertIn("MAX_GPIO_PINS 8", gpio)
        self.assertIn("DEBOUNCE_MS 50", gpio)
        self.assertIn("xQueueReceive", gpio)
        self.assertIn("honch_track(s_mappings[i].event_name, props)", gpio)
        isr = gpio[gpio.index("gpio_isr_handler"):gpio.index("gpio_worker_task")]
        self.assertNotIn("honch_track(", isr)

    def test_component_declares_cbor_dependency(self) -> None:
        manifest = read("ports/esp-idf/honch/idf_component.yml")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn("espressif/cbor", manifest)
        self.assertIn("espressif/cjson", manifest)
        self.assertRegex(cmake, r"\bcbor\b")
        self.assertRegex(cmake, r"\bespressif__cjson\b")
        self.assertRegex(cmake, r"\besp_timer\b")

    def test_transport_sends_application_cbor_with_optional_gzip(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        kconfig = read("ports/esp-idf/honch/Kconfig")

        self.assertIn('"Content-Type", "application/cbor"', transport)
        self.assertNotIn('"Content-Type", "application/json"', transport)
        self.assertIn('"Content-Encoding", "gzip"', transport)
        self.assertIn("CONFIG_HONCH_ENABLE_GZIP", transport)
        self.assertIn("CONFIG_HONCH_GZIP_MIN_BYTES", transport)
        self.assertIn("miniz.h", transport)
        self.assertIn("tdefl_compress_mem_to_heap", transport)
        self.assertNotIn("mz_deflate", transport)
        self.assertIn("compressed_size >= body_size", transport)
        self.assertIn("HONCH_ENABLE_GZIP", kconfig)
        self.assertIn("depends on !IDF_TARGET_ESP32 && !IDF_TARGET_ESP32S2", kconfig)
        self.assertIn("HONCH_GZIP_MIN_BYTES", kconfig)

    def test_dead_pre_core_modules_are_not_part_of_esp_component(self) -> None:
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        kconfig = read("ports/esp-idf/honch/Kconfig")
        bench_defaults = read("ports/esp-idf/benchtest/sdkconfig.defaults")

        for path in (
            "src/honch.c",
            "src/identity.c",
            "src/queue.c",
            "src/encoder.c",
            "src/transport.c",
            "src/scheduler.c",
            "src/lifecycle.c",
        ):
            self.assertFalse((ROOT / "ports/esp-idf/honch" / path).exists())
            self.assertNotIn(f'"{path}"', cmake)

        self.assertNotIn("HONCH_PERF_LOGGING", kconfig)
        self.assertNotIn("HONCH_RAM_QUEUE_DEPTH", kconfig)
        self.assertNotIn("HONCH_DURABLE_IMMEDIATE_NVS", kconfig)
        self.assertNotIn("CONFIG_HONCH_PERF_LOGGING", bench_defaults)

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read("ports/esp-idf/honch/src/esp_storage_nvs.c")
        internal = read("core/src/honch_internal.h")

        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertIn("nvs_set_blob", queue)
        self.assertIn("nvs_get_blob", queue)
        self.assertNotIn("nvs_set_str", queue)
        self.assertNotIn("event_json", internal)

    def test_esp_storage_caps_queue_depth_and_dead_letters_rejected_events(self) -> None:
        queue = read("ports/esp-idf/honch/src/esp_storage_nvs.c")

        self.assertIn("HONCH_ESP_DEFAULT_MAX_QUEUE_DEPTH", queue)
        self.assertIn("honch_esp_queue_drop_oldest(ctx)", queue)
        self.assertIn("honch_esp_queue_dead_letter", queue)
        self.assertIn("nvs_open(HONCH_ESP_DEAD_NAMESPACE", queue)
        self.assertIn(".queue_drop_oldest = honch_esp_queue_drop_oldest", queue)

    def test_encoder_directly_encodes_properties_without_cjson_merge(self) -> None:
        encoder = read("core/src/honch_encoder.c")
        core = read("core/src/honch_core.c")

        self.assertIn("honch_build_event", core)
        self.assertIn("honch_cbor_append_json_object_members", core)
        self.assertIn("honch_property_key_is_reserved", core)
        self.assertNotIn("cJSON_Duplicate", encoder)
        self.assertNotIn("cJSON_AddStringToObject", encoder)
        self.assertNotIn("cJSON_AddNumberToObject", encoder)
        self.assertNotIn("cJSON_DeleteItemFromObject", encoder)

    def test_track_uses_cbor_conversion_as_properties_json_validation(self) -> None:
        core = read("core/src/honch_core.c")
        cbor = read("core/src/honch_cbor.c")

        track_body = core[core.index("honch_status_t honch_core_track"):core.index("honch_status_t honch_core_identify")]
        append_members_body = cbor[cbor.index("honch_status_t honch_cbor_append_json_object_members"):]

        self.assertNotIn("honch_validate_json_object_input(client, properties_json)", track_body)
        self.assertNotIn("honch_json_to_cbor_object(&parser, buffer, 1u, true, member_count)", append_members_body)
        self.assertIn("honch_json_to_cbor_object_members_single_pass", cbor)

    def test_transport_treats_bad_requests_as_non_retryable(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")

        self.assertIn("status == 429", transport)
        self.assertIn("status == 408", transport)
        self.assertLess(transport.index("status == 408"), transport.index("status >= 400 && status < 500"))
        self.assertRegex(transport, r"status >= 400 && status < 500")

    def test_transport_guards_esp_http_post_field_length(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")

        self.assertIn("#include <limits.h>", transport)
        self.assertIn("post_body_size > (size_t)INT_MAX", transport)
        self.assertIn("esp_http_client_set_post_field(client, (const char *)post_body, (int)post_body_size)", transport)
        self.assertLess(
            transport.index("post_body_size > (size_t)INT_MAX"),
            transport.index("esp_http_client_set_post_field"),
        )

    def test_transport_checks_esp_http_setup_failures(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")

        self.assertIn("esp_err_t err = esp_http_client_set_header", transport)
        setup_body = transport[
            transport.index("esp_err_t err = esp_http_client_set_header"):
            transport.index("err = esp_http_client_perform")
        ]
        self.assertIn('err = esp_http_client_set_header(client, "Content-Type", "application/cbor")', setup_body)
        self.assertIn('err = esp_http_client_set_header(client, "Content-Encoding", "gzip")', setup_body)
        self.assertIn("err = esp_http_client_set_post_field", setup_body)
        self.assertGreaterEqual(setup_body.count("if (err != ESP_OK)"), 3)

    def test_gpio_registration_validates_pin_before_shift(self) -> None:
        gpio = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")

        self.assertIn("pin < 0 || pin >= GPIO_NUM_MAX || pin >= 64", gpio)
        self.assertLess(
            gpio.index("pin < 0 || pin >= GPIO_NUM_MAX || pin >= 64"),
            gpio.index("1ULL << pin"),
        )

    def test_benchmark_app_detects_existing_wifi_connection(self) -> None:
        lifecycle = read("ports/esp-idf/benchtest/main/app_main.c")

        self.assertIn("esp_wifi_sta_get_ap_info", lifecycle)
        self.assertIn("Connected to Wi-Fi SSID", lifecycle)

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
        self.assertIn("#include \"esp_http_client.h\"", bench)
        self.assertIn("wait_for_network_ready", bench)
        self.assertIn("esp_wifi_set_ps(WIFI_PS_NONE)", bench)
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
        self.assertIn("ports/esp-idf/**/build-*", gitignore)

    def test_encoder_builds_cbor_epoch_millis_payloads(self) -> None:
        encoder = read("core/src/honch_encoder.c")
        internal = read("core/src/honch_internal.h")
        core = read("core/src/honch_core.c")

        self.assertIn("honch_cbor_append_text", encoder)
        self.assertIn("honch_build_event", core)
        self.assertIn("honch_payload_t", internal)
        self.assertIn("timestamp", core)
        self.assertIn("honch_now_millis", core)
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
