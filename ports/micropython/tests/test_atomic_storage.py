from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class MicroPythonVolatileStorageTests(unittest.TestCase):
    def test_default_storage_uses_core_ram_queue(self) -> None:
        storage = read("ports/micropython/usermod/honch/mpstorage_adapter.c")
        header = read("ports/micropython/usermod/honch/honch_micropython.h")

        self.assertIn("honch_ram_queue_t ram_queue;", header)
        self.assertIn("honch_event_queue_ops_t *ops", storage)
        self.assertIn("honch_ram_queue_init", storage)
        self.assertIn("honch_ram_queue_ops_init", storage)
        self.assertNotIn("MP_QSTR_rename", storage)
        self.assertNotIn("state_directory", storage)
        self.assertNotIn("pending_directory", storage)

    def test_default_state_is_volatile(self) -> None:
        adapter = read("ports/micropython/usermod/honch/mphal_adapter.c")
        config = read("ports/micropython/honch/config.py")
        readme = read("ports/micropython/README.md")
        compact_readme = " ".join(readme.split())

        self.assertNotIn("honch_mp_default_device_id", adapter)
        self.assertNotIn("MP_QSTR_unique_id", adapter)
        self.assertIn('"device_id"', config)
        self.assertIn("`device_id` is required", readme)
        self.assertIn("caller-provided stable device_id", compact_readme)
        self.assertIn("*changed = false;", adapter)
        self.assertNotIn("state_set", adapter)
        self.assertNotIn("state_get", adapter)


if __name__ == "__main__":
    unittest.main()
