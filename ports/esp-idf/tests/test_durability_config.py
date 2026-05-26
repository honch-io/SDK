from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class EspIdfDurabilityConfigTests(unittest.TestCase):
    def test_public_config_exposes_durability_mode(self) -> None:
        header = read("ports/esp-idf/honch/include/honch.h")

        self.assertIn("HONCH_DURABILITY_OS_BUFFERED", header)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS", header)
        self.assertIn("honch_durability_mode_t durability_mode;", header)

    def test_init_forwards_configured_durability_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        self.assertIn("honch_esp_resolve_durability_mode(config->durability_mode)", compat)
        self.assertNotIn("core_config.durability_mode = HONCH_DURABILITY_OS_BUFFERED;", compat)


if __name__ == "__main__":
    unittest.main()
