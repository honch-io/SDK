from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class EspIdfDurabilityConfigTests(unittest.TestCase):
    def test_public_config_omits_durability_mode(self) -> None:
        header = read("ports/esp-idf/honch/include/honch.h")

        self.assertNotIn("HONCH_DURABILITY_OS_BUFFERED", header)
        self.assertNotIn("HONCH_DURABILITY_SYNC_ALWAYS", header)
        self.assertNotIn("durability_mode", header)

    def test_init_uses_explicit_event_queue_extension_point(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        self.assertIn("config->event_queue_ops", compat)
        self.assertIn("config->state_storage_ops", compat)
        self.assertNotIn("honch_esp_resolve_durability_mode", compat)

    def test_readme_documents_volatile_default(self) -> None:
        readme = read("ports/esp-idf/README.md")

        self.assertIn("RAM-only by default", readme)
        self.assertIn("event_queue_ops", readme)
        self.assertNotIn("durability_mode", readme)


if __name__ == "__main__":
    unittest.main()
