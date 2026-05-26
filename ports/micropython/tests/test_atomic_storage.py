from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class MicroPythonAtomicStorageTests(unittest.TestCase):
    def test_state_and_queue_writes_use_atomic_helper(self) -> None:
        storage = read("ports/micropython/usermod/honch/mpstorage_adapter.c")

        self.assertIn("honch_mp_write_file_atomic", storage)
        self.assertIn("honch_mp_temp_path", storage)
        self.assertIn("MP_QSTR_rename", storage)
        self.assertIn("status = honch_mp_write_file_atomic(path, data, data_size);", storage)
        self.assertIn("status = honch_mp_write_file_atomic(path, event, event_size);", storage)
        self.assertNotIn("status = honch_mp_write_file(path, data, data_size);", storage)
        self.assertNotIn("status = honch_mp_write_file(path, event, event_size);", storage)

    def test_atomic_helper_cleans_up_temp_file_on_failure(self) -> None:
        storage = read("ports/micropython/usermod/honch/mpstorage_adapter.c")

        self.assertIn("(void)honch_mp_call_os1(MP_QSTR_remove, temp_path);", storage)


if __name__ == "__main__":
    unittest.main()
