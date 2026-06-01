from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class ArduinoTLSConfigTests(unittest.TestCase):
    def test_public_config_exposes_root_ca_pem(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/honch_arduino_adapter.h")

        self.assertIn("const char *rootCaPem;", header)
        self.assertIn("const char *rootCaPem;", adapter)

    def test_secure_transport_uses_root_ca_before_insecure_fallback(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")

        self.assertIn("secureClient.setCACert(transport->rootCaPem);", transport)
        self.assertLess(
            transport.index("secureClient.setCACert(transport->rootCaPem);"),
            transport.index("secureClient.setInsecure();"),
        )

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

        self.assertIn("uint32_t flushMinIntervalMs;", header)
        self.assertIn("coreConfig.flush_min_interval_ms = config.flushMinIntervalMs;", adapter)
        self.assertIn("flushMinIntervalMs", readme)
        self.assertIn("HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", readme)

    def test_transport_applies_configured_timeout(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        transport_header = read("ports/arduino/src/honch_arduino_adapter.h")
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("uint32_t transportTimeoutMs;", header)
        self.assertIn("coreConfig.transport_timeout_ms = config.transportTimeoutMs;", adapter)
        self.assertIn("uint32_t transportTimeoutMs;", transport_header)
        self.assertIn("HONCH_ARDUINO_DEFAULT_TRANSPORT_TIMEOUT_MS 3000u", transport)
        self.assertIn(
            "ctx->transportTimeoutMs = config.transportTimeoutMs == 0u ?",
            transport,
        )
        self.assertIn("http.setTimeout(transport->transportTimeoutMs);", transport)
        self.assertLess(
            transport.index("http.setTimeout(transport->transportTimeoutMs);"),
            transport.index("int code = http.POST("),
        )
        self.assertIn("transportTimeoutMs", readme)

    def test_public_config_exposes_connectivity_gate(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("bool (*connectivityCallback)();", header)
        self.assertIn("honch_arduino_connectivity_callback", adapter)
        self.assertIn("coreConfig.connectivity_callback = honch_arduino_connectivity_callback;", adapter)
        self.assertIn("connectivityCallback", readme)
        self.assertIn("offline", readme)

    def test_tick_contract_warns_about_blocking_and_shows_dedicated_task(self) -> None:
        readme = read("ports/arduino/README.md")
        task_example = read("ports/arduino/examples/HonchDedicatedTask/HonchDedicatedTask.ino")
        normalized_readme = " ".join(readme.split())
        normalized_example = " ".join(task_example.split())

        self.assertIn("Honch.tick() may block for up to the configured transport timeout", normalized_readme)
        self.assertIn("Do not call Honch.tick() from a latency-sensitive control loop", normalized_readme)
        self.assertIn("xTaskCreatePinnedToCore", readme)
        self.assertRegex(task_example, r"xTaskCreatePinnedToCore\(\s*honchPumpTask")
        self.assertIn("Honch.tick();", task_example)
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
        self.assertIn("#define HONCH_AUTO_PROPERTY_BUFFER_COUNT 1u", internal)
        self.assertNotIn("atomic_bool auto_property_buffer_in_use", internal)
        self.assertIn("honch_atomic_bool_t auto_property_buffer_in_use", internal)
        self.assertIn("honch_atomic_bool_compare_exchange", core)
        self.assertIn("honch_atomic_bool_store", core)


if __name__ == "__main__":
    unittest.main()
