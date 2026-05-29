from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def c_function_body(source: str, name: str) -> str:
    marker = f"{name}("
    signature_start = source.find(marker)
    if signature_start < 0:
        raise AssertionError(f"function {name} not found")

    open_brace = source.find("{", signature_start)
    if open_brace < 0:
        raise AssertionError(f"function {name} body not found")

    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace + 1:index]

    raise AssertionError(f"function {name} body is not closed")


class EspIdfSingletonLifetimeTests(unittest.TestCase):
    def test_core_shutdown_frees_client_storage(self) -> None:
        core = read("core/src/honch_core.c")
        shutdown = c_function_body(core, "honch_core_shutdown")

        self.assertIn("free(client);", shutdown)

    def test_esp_shutdown_detaches_singleton_before_freeing_client(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        shutdown = c_function_body(compat, "honch_shutdown")
        detach = c_function_body(compat, "honch_esp_client_detach")

        detach_index = shutdown.find("honch_esp_client_detach(&client)")
        shutdown_index = shutdown.find("honch_core_shutdown(")

        self.assertIn("s_client = NULL;", detach)
        self.assertGreaterEqual(detach_index, 0)
        self.assertGreaterEqual(shutdown_index, 0)
        self.assertLess(detach_index, shutdown_index)

    def test_esp_singleton_uses_port_lifetime_gate(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        self.assertIn("static pthread_mutex_t s_client_mutex", compat)
        self.assertIn("honch_esp_client_acquire", compat)
        self.assertIn("honch_client_enter(client)", compat)
        self.assertIn("honch_esp_client_release", compat)
        self.assertIn("honch_client_leave(client)", compat)
        self.assertIn("honch_esp_client_detach", compat)

    def test_public_wrappers_do_not_pass_global_singleton_directly_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        unsafe_calls = [
            "honch_core_track(s_client",
            "honch_core_identify(s_client",
            "honch_core_set_property(s_client",
            "honch_core_session_start(s_client",
            "honch_core_session_end(s_client",
            "honch_core_flush(s_client",
            "honch_core_reset(s_client",
            "honch_core_get_device_id(s_client",
        ]

        present = [call for call in unsafe_calls if call in compat]

        self.assertEqual([], present)

    def test_init_is_blocked_until_detached_client_shutdown_finishes(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        init_begin = c_function_body(compat, "honch_esp_init_begin")
        detach = c_function_body(compat, "honch_esp_client_detach")
        shutdown = c_function_body(compat, "honch_shutdown")
        shutdown_finish = c_function_body(compat, "honch_esp_shutdown_finish")

        self.assertIn("static bool s_client_shutting_down", compat)
        self.assertIn("s_client_shutting_down", init_begin)
        self.assertRegex(
            init_begin,
            r"if\s*\([^)]*s_client\s*!=\s*NULL[^)]*s_client_initializing[^)]*s_client_shutting_down",
        )
        self.assertIn("s_client_shutting_down = true;", detach)
        self.assertIn("s_client = NULL;", detach)
        self.assertIn("s_client_shutting_down = false;", shutdown_finish)

        core_shutdown_index = shutdown.find("honch_core_shutdown(client)")
        finish_index = shutdown.find("honch_esp_shutdown_finish()")
        self.assertGreaterEqual(core_shutdown_index, 0)
        self.assertGreater(finish_index, core_shutdown_index)

    def test_state_reset_errors_after_identity_snapshot_use_cleanup_path(self) -> None:
        shims = read("ports/esp-idf/honch/src/esp_core_shims.c")
        reset = c_function_body(shims, "honch_state_reset")

        snapshot_index = reset.find("previous_distinct_id = honch_strdup")
        cleanup_index = reset.find("cleanup:")
        self.assertGreaterEqual(snapshot_index, 0)
        self.assertGreater(
            cleanup_index,
            snapshot_index,
            "reset must have a shared cleanup path after previous identities are copied",
        )

        post_snapshot = reset[snapshot_index:cleanup_index]
        early_returns = [
            line.strip()
            for line in post_snapshot.splitlines()
            if line.strip().startswith("return ")
        ]
        self.assertEqual(
            [],
            early_returns,
            "post-snapshot reset failures must not return before freeing previous identities",
        )


if __name__ == "__main__":
    unittest.main()
