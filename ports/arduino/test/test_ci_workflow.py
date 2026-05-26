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
        self.assertIn("--build-path", script)
        self.assertIn("arduino-cli not found; install arduino-cli or omit --require-arduino-cli", script)

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
        self.assertIn("HONCH_ARDUINO_HOME:", workflow)
        self.assertIn("ports/arduino/scripts/verify-arduino.sh --require-arduino-cli", workflow)


if __name__ == "__main__":
    unittest.main()
