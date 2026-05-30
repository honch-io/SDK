from pathlib import Path
import re
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

    def test_storage_init_cleans_orphaned_temp_files(self) -> None:
        storage = read("ports/micropython/usermod/honch/mpstorage_adapter.c")

        self.assertIn("honch_mp_cleanup_tmp_files", storage)
        self.assertIn("honch_mp_name_has_suffix(name, name_size, \".tmp\")", storage)
        self.assertIn("status = honch_mp_cleanup_tmp_files(ctx->pending_directory);", storage)
        self.assertIn("status = honch_mp_cleanup_tmp_files(ctx->dead_directory);", storage)
        self.assertIn("status = honch_mp_cleanup_tmp_files(ctx->state_directory);", storage)

    def test_queue_mutations_reset_peek_cursor(self) -> None:
        storage = read("ports/micropython/usermod/honch/mpstorage_adapter.c")

        for fn_name in (
            "honch_mp_queue_consume",
            "honch_mp_queue_dead_letter",
            "honch_mp_queue_drop_oldest",
            "honch_mp_queue_clear",
        ):
            match = re.search(
                r"static honch_status_t " + fn_name + r"\([^)]*\)\n\{(?P<body>.*?)\n\}",
                storage,
                re.DOTALL,
            )
            self.assertIsNotNone(match, fn_name)
            self.assertIn("active_sequence = UINT64_MAX", match.group("body"), fn_name)


if __name__ == "__main__":
    unittest.main()
