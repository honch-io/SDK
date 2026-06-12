from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


SHARED_ESP_RESET_REASONS = (
    "ESP_RST_POWERON",
    "ESP_RST_SW",
    "ESP_RST_DEEPSLEEP",
    "ESP_RST_PANIC",
    "ESP_RST_INT_WDT",
    "ESP_RST_TASK_WDT",
    "ESP_RST_WDT",
    "ESP_RST_BROWNOUT",
    "ESP_RST_SDIO",
    "ESP_RST_EXT",
    "ESP_RST_USB",
    "ESP_RST_JTAG",
    "ESP_RST_EFUSE",
    "ESP_RST_PWR_GLITCH",
    "ESP_RST_UNKNOWN",
)


def reset_case_body(source: str, reset_reason: str) -> str:
    case_start = source.find(f"case {reset_reason}:")
    if case_start < 0:
        raise AssertionError(f"{reset_reason} not found")
    if reset_reason == "ESP_RST_UNKNOWN":
        return source[case_start:]

    case_labels = re.finditer(r"^\s*(case\s+[A-Z0-9_]+:|default:)", source, re.MULTILINE)
    for label in case_labels:
        if label.start() > case_start:
            return source[case_start:label.start()]
    return source[case_start:]


def arduino_reset_mapping(source: str, reset_reason: str) -> tuple[str, str, str]:
    body = reset_case_body(source, reset_reason)
    match = re.search(
        r"honch_arduino_reset_snapshot\(\s*"
        r"(HONCH_FAULT_KIND_[A-Z_]+),\s*"
        r"(HONCH_FAULT_SEVERITY_[A-Z_]+),\s*"
        r'"([^"]+)"\s*\)',
        body,
    )
    if match is None:
        raise AssertionError(f"{reset_reason} Arduino mapping not parsed")
    return match.group(1), match.group(2), match.group(3)


def esp_idf_reset_mapping(source: str, reset_reason: str) -> tuple[str, str, str]:
    body = reset_case_body(source, reset_reason)
    abnormal = re.search(
        r"honch_esp_abnormal_fault_snapshot\(\s*"
        r"(HONCH_FAULT_KIND_[A-Z_]+),\s*"
        r'"([^"]+)"',
        body,
    )
    if abnormal is not None:
        return abnormal.group(1), "HONCH_FAULT_SEVERITY_FATAL", abnormal.group(2)

    direct = re.search(
        r"\.kind\s*=\s*(HONCH_FAULT_KIND_[A-Z_]+).*?"
        r"\.severity\s*=\s*(HONCH_FAULT_SEVERITY_[A-Z_]+).*?"
        r'\.reset_reason\s*=\s*"([^"]+)"',
        body,
        re.DOTALL,
    )
    if direct is None:
        raise AssertionError(f"{reset_reason} ESP-IDF mapping not parsed")
    return direct.group(1), direct.group(2), direct.group(3)


class ArduinoTLSConfigTests(unittest.TestCase):
    def test_public_config_exposes_root_ca_pem(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/honch_arduino_adapter.h")

        self.assertIn("const char *rootCaPem = nullptr;", header)
        self.assertIn("const char *rootCaPem;", adapter)

    def test_secure_transport_uses_root_ca_before_insecure_fallback(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")

        self.assertIn("secureClient->setCACert(transport->rootCaPem);", transport)
        self.assertLess(
            transport.index("secureClient->setCACert(transport->rootCaPem);"),
            transport.index("secureClient->setInsecure();"),
        )

    def test_secure_transport_does_not_put_tls_client_on_caller_stack(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")
        readme = read("ports/arduino/README.md")
        task_example = read("ports/arduino/examples/HonchDedicatedTask/HonchDedicatedTask.ino")

        self.assertNotIn("WiFiClientSecure secureClient;", transport)
        self.assertIn("new (std::nothrow) WiFiClientSecure()", transport)
        self.assertIn("delete secureClient;", transport)
        self.assertIn("HTTPS uploads allocate the TLS client lazily on the heap", readme)
        self.assertIn("8192", readme)
        self.assertIn("8192", task_example)

    def test_examples_configure_tls_root_ca_not_insecure_skip(self) -> None:
        basic = read("ports/arduino/examples/HonchBasic/HonchBasic.ino")
        offline = read("ports/arduino/examples/HonchOfflineQueue/HonchOfflineQueue.ino")

        for sketch in (basic, offline):
            self.assertIn("static const char HONCH_ROOT_CA_PEM[]", sketch)
            self.assertIn("config.rootCaPem = HONCH_ROOT_CA_PEM;", sketch)
            self.assertIn("config.insecureSkipTlsVerify = false;", sketch)

    def test_public_config_exposes_flush_spacing(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("uint32_t flushMinIntervalMs = 0;", header)
        self.assertIn("coreConfig.flush_min_interval_ms = config.flushMinIntervalMs;", adapter)
        self.assertIn("flushMinIntervalMs", readme)
        self.assertIn("HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", readme)

    def test_transport_applies_configured_timeout(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        transport_header = read("ports/arduino/src/honch_arduino_adapter.h")
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("uint32_t transportTimeoutMs = 0;", header)
        self.assertIn("coreConfig.transport_timeout_ms = config.transportTimeoutMs;", adapter)
        self.assertIn("uint32_t transportTimeoutMs;", transport_header)
        self.assertIn("HONCH_ARDUINO_DEFAULT_TRANSPORT_TIMEOUT_MS 3000u", transport)
        self.assertIn("HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS 10000u", transport)
        self.assertIn(
            "ctx->transportTimeoutMs = config.transportTimeoutMs == 0u ?",
            transport,
        )
        self.assertIn("if (ctx->transportTimeoutMs > HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS)", transport)
        self.assertIn("ctx->transportTimeoutMs = HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS;", transport)
        self.assertIn("http.setTimeout(transport->transportTimeoutMs);", transport)
        self.assertLess(
            transport.index("http.setTimeout(transport->transportTimeoutMs);"),
            transport.index("int code = http.POST("),
        )
        self.assertIn("transportTimeoutMs", readme)
        self.assertIn("maximum of 10000 ms", readme)

    def test_public_config_exposes_connectivity_gate(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("bool (*connectivityCallback)() = nullptr;", header)
        self.assertIn("honch_arduino_connectivity_callback", adapter)
        self.assertIn("coreConfig.connectivity_callback = honch_arduino_connectivity_callback;", adapter)
        self.assertIn("connectivityCallback", readme)
        self.assertIn("offline", readme)

    def test_default_client_is_namespaced_not_unprefixed_global(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertNotIn("extern HonchClass Honch;", header)
        self.assertNotIn("HonchClass Honch;", adapter)
        self.assertIn("namespace honch", header)
        self.assertIn("HonchClass &defaultClient();", header)
        self.assertIn("HonchClass g_defaultClient;", adapter)
        self.assertIn("HonchClass &defaultClient()", adapter)
        self.assertIn("honch::defaultClient()", readme)

    def test_readme_explains_wrapper_and_core_runtime_versions(self) -> None:
        readme = read("ports/arduino/README.md")
        library = read("ports/arduino/library.properties")
        internal = read("ports/arduino/src/honch_internal.h")
        normalized = " ".join(readme.split())

        self.assertIn("version=0.1.0", library)
        self.assertIn('#define HONCH_SDK_VERSION "0.2.0"', internal)
        self.assertIn("Arduino wrapper/package version", normalized)
        self.assertIn("0.1.0", normalized)
        self.assertIn("shared C core runtime version", normalized)
        self.assertIn("0.2.0", normalized)

    def test_tick_contract_warns_about_blocking_and_shows_dedicated_task(self) -> None:
        readme = read("ports/arduino/README.md")
        task_example = read("ports/arduino/examples/HonchDedicatedTask/HonchDedicatedTask.ino")
        normalized_readme = " ".join(readme.split())
        normalized_example = " ".join(task_example.split())

        self.assertIn("`honch::defaultClient().tick()` may block for up to the configured transport timeout", normalized_readme)
        self.assertIn("Do not call `honch::defaultClient().tick()` from a latency-sensitive control loop", normalized_readme)
        self.assertIn("Do not call `honch::defaultClient().tick()` or `honch::defaultClient().flush()` from an ISR", normalized_readme)
        self.assertIn("high-priority task", normalized_readme)
        self.assertIn("xTaskCreatePinnedToCore", readme)
        self.assertRegex(task_example, r"xTaskCreatePinnedToCore\(\s*honchPumpTask")
        self.assertIn("honch::defaultClient().tick();", task_example)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(250));", task_example)

    def test_transport_initializer_matches_core_ops_shape(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")

        self.assertIn("arduino_post_chunk,\n      nullptr,\n      ctx,", transport)

    def test_arduino_shared_locks_are_bounded_and_fail_open(self) -> None:
        platform = read("ports/arduino/src/honch_arduino_platform.cpp")

        self.assertNotIn("portMAX_DELAY", platform)
        self.assertIn("HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS", platform)
        self.assertIn("pdMS_TO_TICKS(HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS)", platform)
        self.assertIn("try_lock_for(", platform)
        self.assertIn("std::chrono::milliseconds(HONCH_ARDUINO_MUTEX_LOCK_TIMEOUT_MS)", platform)
        self.assertIn("HONCH_ERROR_BUSY", platform)

    def test_vendored_core_hides_atomic_storage_from_cxx_builds(self) -> None:
        internal = read("ports/arduino/src/honch_internal.h")
        core = read("ports/arduino/src/honch_core.c")

        self.assertNotIn("#include <stdatomic.h>", internal)
        self.assertNotIn("atomic_bool auto_property_buffer_in_use", internal)
        self.assertIn("honch_atomic_bool_t auto_property_buffer_in_use", internal)
        self.assertIn("honch_atomic_bool_compare_exchange", core)
        self.assertIn("honch_atomic_bool_store", core)

    def test_arduino_passes_reset_reason_snapshot_to_core(self) -> None:
        adapter = read("ports/arduino/src/honch_arduino_adapter.h")
        platform = read("ports/arduino/src/honch_arduino_platform.cpp")
        wrapper = read("ports/arduino/src/Honch.cpp")
        public = read("ports/arduino/src/Honch.h")

        self.assertIn("bool enableErrorTracking = false;", public)
        self.assertIn("honch_fault_snapshot_t honch_arduino_fault_snapshot()", adapter)
        self.assertIn("#include <esp_system.h>", platform)
        self.assertIn("esp_reset_reason()", platform)
        self.assertIn("ESP_RST_PANIC", platform)
        self.assertIn("ESP_RST_TASK_WDT", platform)
        self.assertIn("ESP_RST_INT_WDT", platform)
        self.assertIn("ESP_RST_BROWNOUT", platform)
        self.assertIn("HONCH_FAULT_KIND_PANIC", platform)
        self.assertIn("HONCH_FAULT_KIND_WATCHDOG", platform)
        self.assertIn("HONCH_FAULT_KIND_BROWNOUT", platform)
        self.assertIn("honch_fault_snapshot_t gFaultSnapshot;", wrapper)
        self.assertIn("gFaultSnapshot = honch_arduino_fault_snapshot();", wrapper)
        self.assertIn("coreConfig.enable_error_tracking = config.enableErrorTracking;", wrapper)
        self.assertIn("coreConfig.fault_snapshot = &gFaultSnapshot;", wrapper)

    def test_arduino_maps_idf_5_1_reset_reasons_without_fatal_unknown(self) -> None:
        platform = read("ports/arduino/src/honch_arduino_platform.cpp")

        for reset_reason, reset_string in (
            ("ESP_RST_USB", '"usb"'),
            ("ESP_RST_JTAG", '"jtag"'),
            ("ESP_RST_EFUSE", '"efuse"'),
        ):
            self.assertIn(reset_reason, platform)
            self.assertIn(reset_string, platform)

        self.assertIn("ESP_RST_PWR_GLITCH", platform)
        self.assertIn('"power_glitch"', platform)
        self.assertIn("HONCH_FAULT_KIND_BROWNOUT", platform)
        self.assertIn("ESP_IDF_VERSION_VAL(5, 1, 0)", platform)

    def test_arduino_reset_mapping_matches_esp_idf_port(self) -> None:
        arduino_platform = read("ports/arduino/src/honch_arduino_platform.cpp")
        esp_platform = read("ports/esp-idf/honch/src/esp_platform.c")

        for reset_reason in SHARED_ESP_RESET_REASONS:
            self.assertEqual(
                esp_idf_reset_mapping(esp_platform, reset_reason),
                arduino_reset_mapping(arduino_platform, reset_reason),
                reset_reason,
            )

    def test_arduino_error_tracking_can_be_compiled_out(self) -> None:
        adapter = read("ports/arduino/src/honch_arduino_adapter.h")
        platform = read("ports/arduino/src/honch_arduino_platform.cpp")
        wrapper = read("ports/arduino/src/Honch.cpp")
        core = read("ports/arduino/src/honch_core.c")
        config = read("ports/arduino/src/honch/core/config.h")

        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", adapter)
        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", platform)
        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", wrapper)
        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", core)
        self.assertIn("#ifndef HONCH_ENABLE_ERROR_TRACKING", config)


if __name__ == "__main__":
    unittest.main()
