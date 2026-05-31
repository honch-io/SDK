from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class EspGpioHostOwnedExampleTests(unittest.TestCase):
    def test_core_sdk_does_not_expose_or_compile_gpio_tracking(self) -> None:
        public_header = read("ports/esp-idf/honch/include/honch.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        combined = public_header + cmake + compat
        self.assertNotIn("honch_track_gpio", combined)
        self.assertNotIn("honch_gpio_mode_t", combined)
        self.assertNotIn("HONCH_GPIO_", combined)
        self.assertNotIn("driver/gpio.h", public_header)
        self.assertNotIn("esp_gpio_adapter", cmake)
        self.assertNotIn("esp_gpio_public", cmake)
        self.assertNotIn("esp_driver_gpio", cmake)
        self.assertNotIn("honch_esp_gpio_shutdown_hook", compat)

    def test_default_examples_do_not_use_core_gpio_tracking(self) -> None:
        for path in (
            "ports/esp-idf/example/main/app_main.c",
            "ports/esp-idf/benchtest/main/app_main.c",
            "ports/esp-idf/footprint/main/app_main.c",
            "ports/esp-idf/README.md",
        ):
            content = read(path)
            self.assertNotIn("honch_track_gpio", content, path)
            self.assertNotIn("HONCH_GPIO_", content, path)

    def test_gpio_example_owns_gpio_setup_and_tracks_from_task_context(self) -> None:
        example = read("ports/esp-idf/example_gpio/main/app_main.c")
        readme = read("ports/esp-idf/example_gpio/README.md")

        self.assertIn('#include "driver/gpio.h"', example)
        self.assertIn("gpio_config(&io_conf)", example)
        self.assertIn("gpio_install_isr_service", example)
        self.assertIn("gpio_isr_handler_add", example)
        self.assertIn("xQueueSendFromISR", example)
        self.assertIn("honch_track(\"button_pressed\"", example)
        self.assertIn("host-owned", readme)
        self.assertIn("does not configure GPIO", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
