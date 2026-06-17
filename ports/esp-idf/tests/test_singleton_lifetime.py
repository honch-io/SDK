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

        self.assertIn("static SemaphoreHandle_t s_client_mutex", compat)
        self.assertIn("HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS", compat)
        self.assertIn("static honch_err_t honch_esp_client_lock(void)", compat)
        self.assertIn("return HONCH_ERR_BUSY;", c_function_body(compat, "honch_esp_client_lock"))
        self.assertIn("honch_esp_client_acquire", compat)
        self.assertIn("honch_client_enter(client)", compat)
        self.assertIn("honch_esp_client_release", compat)
        self.assertIn("honch_client_leave(client)", compat)
        self.assertIn("honch_esp_client_detach", compat)

    def test_esp_compat_does_not_keep_legacy_global_config_mirrors(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        for name in (
            "g_honch_api_key",
            "g_honch_device_model",
            "g_honch_firmware_version",
            "g_honch_environment",
            "g_honch_battery_callback",
            "g_honch_battery_low_threshold",
            "g_honch_connected",
        ):
            self.assertNotIn(name, compat)

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

    def test_state_transition_finalizers_cannot_fail_open(self) -> None:
        # A try-lock timeout in a finalizer must not strand s_client_initializing
        # / s_client_shutting_down set (which would brick honch_init for the rest
        # of the boot). The finalizers use a blocking lock that always completes.
        compat = read("ports/esp-idf/honch/src/esp_compat.c")

        lock_blocking = c_function_body(compat, "honch_esp_client_lock_blocking")
        self.assertIn("portMAX_DELAY", lock_blocking)

        for name in (
            "honch_esp_init_finish",
            "honch_esp_shutdown_finish",
            "honch_esp_shutdown_restore",
        ):
            body = c_function_body(compat, name)
            self.assertIn("honch_esp_client_lock_blocking()", body)
            # No cooperative try-lock and no fail-open early return in a finalizer.
            self.assertNotIn("honch_esp_client_lock()", body)
            self.assertNotRegex(body, r"!=\s*HONCH_OK")
            self.assertNotIn("return err;", body)

    def test_state_reset_restores_distinct_id_to_device_id(self) -> None:
        shims = read("ports/esp-idf/honch/src/esp_core_shims.c")
        reset = c_function_body(shims, "honch_state_reset")

        self.assertIn("char *distinct_id = honch_strdup(client->device_id);", reset)
        self.assertIn("honch_state_save_distinct_id_value(client, distinct_id)", reset)
        self.assertIn("client->distinct_id = distinct_id;", reset)
        self.assertNotIn("honch_random_hex", reset)


if __name__ == "__main__":
    unittest.main()
