from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class ArduinoCIWorkflowTests(unittest.TestCase):
    def test_verify_script_has_strict_arduino_cli_mode(self) -> None:
        script = read("ports/arduino/scripts/verify-arduino.sh")

        self.assertIn("--require-arduino-cli", script)
        self.assertIn("HONCH_REQUIRE_ARDUINO_CLI", script)
        self.assertIn("check-core-sync.sh", script)
        self.assertIn("--build-path", script)
        self.assertIn("arduino-cli not found; install arduino-cli or omit --require-arduino-cli", script)
        self.assertIn("arduino-cli core list", script)
        self.assertIn("Arduino ESP32 platform not installed; skipped Arduino example compile checks", script)
        self.assertIn("arduino-cli core install esp32:esp32", script)

    def test_vendored_core_sync_script_checks_sources_and_headers(self) -> None:
        script = read("ports/arduino/scripts/check-core-sync.sh")

        self.assertIn("core/src/honch_*.c", script)
        self.assertIn("core/src/honch_internal.h", script)
        self.assertIn("core/include/honch/core/*.h", script)
        self.assertIn("cmp -s", script)
        self.assertIn("differs from", script)
        # Bidirectional: also detects a stale vendored copy with no core source.
        self.assertIn("orphan vendored", script)
        self.assertIn("ports/arduino/src/honch_*.c", script)

    def test_verify_script_compiles_supported_esp32_families(self) -> None:
        script = read("ports/arduino/scripts/verify-arduino.sh")

        self.assertIn("esp32:esp32:esp32", script)
        self.assertIn("esp32:esp32:esp32s2", script)
        self.assertIn("esp32:esp32:esp32s3", script)
        self.assertIn("esp32:esp32:esp32c3", script)

    def test_github_workflow_runs_strict_arduino_verification(self) -> None:
        workflow = read(".github/workflows/arduino.yml")

        self.assertIn("Arduino ESP32 SDK", workflow)
        self.assertIn("arduino/setup-arduino-cli", workflow)
        self.assertIn("arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json", workflow)
        self.assertIn("arduino-cli core install esp32:esp32", workflow)
        self.assertNotIn("runner.temp", workflow)
        self.assertIn("HONCH_ARDUINO_HOME=$RUNNER_TEMP/arduino", workflow)
        self.assertIn("ports/arduino/scripts/verify-arduino.sh --require-arduino-cli", workflow)

    def test_platformio_manifest_declares_arduino_esp32_library(self) -> None:
        manifest = read("ports/arduino/library.json")

        self.assertIn('"name": "Honch"', manifest)
        self.assertIn('"license": "Apache-2.0"', manifest)
        self.assertIn('"frameworks": "arduino"', manifest)
        self.assertIn('"platforms": "espressif32"', manifest)
        self.assertIn('"srcDir": "src"', manifest)
        self.assertIn('"examples"', manifest)

    def test_arduino_events_are_ram_first_and_durability_default_is_buffered(self) -> None:
        storage = read("ports/arduino/src/honch_arduino_storage.cpp")
        config = read("ports/arduino/src/honch/core/config.h")

        self.assertIn("honch_ram_queue_init(&ctx->ramQueue", storage)
        self.assertIn("honch_ram_queue_ops_init(ops, &ctx->ramQueue)", storage)
        self.assertNotIn("Preferences", storage)
        self.assertNotIn("nvs_", storage.lower())
        self.assertIn("HONCH_DURABILITY_OS_BUFFERED = 0", config)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS = 1", config)
        self.assertIn("HONCH_DURABILITY_DEFAULT = HONCH_DURABILITY_OS_BUFFERED", config)

    def test_platformio_example_build_is_in_ci(self) -> None:
        workflow = read(".github/workflows/arduino.yml")
        platformio = read("ports/arduino/examples/platformio/platformio.ini")

        self.assertIn("Install PlatformIO", workflow)
        self.assertIn("pio run -d ports/arduino/examples/platformio", workflow)
        self.assertIn("lib_extra_dirs = ../..", platformio)
        self.assertIn("src_dir = ../HonchBasic", platformio)
        self.assertIn("board = esp32dev", platformio)


if __name__ == "__main__":
    unittest.main()
