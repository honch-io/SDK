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


if __name__ == "__main__":
    unittest.main()
