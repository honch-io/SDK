from pathlib import Path
import re
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


class EspGpioRegistrationTests(unittest.TestCase):
    def test_gpio_register_rejects_unknown_mode(self) -> None:
        adapter = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        register = c_function_body(adapter, "honch_gpio_register")

        self.assertRegex(
            register,
            r"default:\s*return\s+HONCH_ERR_INVALID_ARG;",
        )

    def test_gpio_register_stages_mapping_until_after_fallible_setup(self) -> None:
        adapter = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        register = c_function_body(adapter, "honch_gpio_register")

        first_mapping_write = register.find("s_mappings[idx].pin")
        handler_add = register.find("gpio_isr_handler_add")

        self.assertGreaterEqual(first_mapping_write, 0)
        self.assertGreaterEqual(handler_add, 0)
        self.assertGreater(
            first_mapping_write,
            handler_add,
            "mapping table should be committed only after GPIO setup succeeds",
        )

    def test_gpio_register_rolls_back_only_appended_mapping_slots(self) -> None:
        adapter = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        register = c_function_body(adapter, "honch_gpio_register")

        self.assertIn("bool added_mapping", register)
        self.assertIn("added_mapping = true;", register)
        self.assertNotIn("s_mapping_count--;", register)
        self.assertIn("honch_gpio_rollback_mapping", adapter)
        self.assertRegex(
            register,
            r"if\s*\(\s*err\s*!=\s*ESP_OK\s*\)\s*\{[^}]*honch_gpio_rollback_mapping",
        )

    def test_gpio_register_restores_replaced_handler_on_add_failure(self) -> None:
        adapter = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        register = c_function_body(adapter, "honch_gpio_register")

        remove_index = register.find("gpio_isr_handler_remove(pin)")
        add_index = register.find("gpio_isr_handler_add(pin, gpio_isr_handler")
        failure_index = register.find("Failed to add ISR handler")
        restore_index = register.find("honch_gpio_restore_replaced_handler")

        self.assertGreaterEqual(remove_index, 0)
        self.assertGreaterEqual(add_index, 0)
        self.assertGreaterEqual(failure_index, 0)
        self.assertGreaterEqual(restore_index, 0)
        self.assertLess(remove_index, add_index)
        self.assertGreater(restore_index, failure_index)

    def test_gpio_mapping_state_is_synchronized_without_tracking_under_lock(self) -> None:
        adapter = read("ports/esp-idf/honch/src/esp_gpio_adapter.c")
        init = c_function_body(adapter, "honch_gpio_init")
        deinit = c_function_body(adapter, "honch_gpio_deinit")
        register = c_function_body(adapter, "honch_gpio_register")
        worker = c_function_body(adapter, "gpio_worker_task")

        self.assertIn("static SemaphoreHandle_t s_mapping_mutex", adapter)
        self.assertIn("xSemaphoreCreateMutex()", init)
        self.assertIn("vSemaphoreDelete(s_mapping_mutex)", deinit)
        self.assertIn("HONCH_ESP_GPIO_LOCK_TIMEOUT_MS", adapter)
        self.assertIn("HONCH_ESP_GPIO_WORKER_LOCK_TIMEOUT_MS", adapter)
        self.assertIn("honch_gpio_mapping_lock(honch_gpio_ticks(HONCH_ESP_GPIO_LOCK_TIMEOUT_MS))", register)
        self.assertIn("honch_gpio_mapping_unlock()", register)
        self.assertIn("honch_gpio_mapping_lock(honch_gpio_ticks(HONCH_ESP_GPIO_LOCK_TIMEOUT_MS))", deinit)
        self.assertIn("honch_gpio_mapping_lock(honch_gpio_ticks(HONCH_ESP_GPIO_WORKER_LOCK_TIMEOUT_MS))", worker)
        self.assertNotIn("portMAX_DELAY", adapter)

        track_index = worker.find("honch_track(event_name, properties, 1u)")
        unlock_index = worker.rfind("honch_gpio_mapping_unlock()", 0, track_index)
        event_copy_index = worker.find("strncpy(event_name, s_mappings[i].event_name")

        self.assertGreaterEqual(track_index, 0)
        self.assertGreaterEqual(unlock_index, 0)
        self.assertGreaterEqual(event_copy_index, 0)
        self.assertLess(event_copy_index, unlock_index)
        self.assertLess(unlock_index, track_index)

    def test_gpio_public_lifecycle_flag_is_synchronized(self) -> None:
        public = read("ports/esp-idf/honch/src/esp_gpio_public.c")
        track_gpio = c_function_body(public, "honch_track_gpio")
        shutdown_hook = c_function_body(public, "honch_esp_gpio_shutdown_hook")

        self.assertIn("#include \"freertos/semphr.h\"", public)
        self.assertIn("static SemaphoreHandle_t s_gpio_lifecycle_mutex", public)
        self.assertIn("honch_gpio_lifecycle_lock()", public)
        self.assertIn("honch_gpio_lifecycle_unlock()", public)
        self.assertIn("honch_gpio_lifecycle_lock()", track_gpio)
        self.assertIn("honch_gpio_lifecycle_unlock()", track_gpio)
        self.assertIn("honch_gpio_lifecycle_lock()", shutdown_hook)
        self.assertIn("honch_gpio_lifecycle_unlock()", shutdown_hook)

        track_lock_index = track_gpio.find("honch_gpio_lifecycle_lock()")
        init_check_index = track_gpio.find("if (!s_gpio_initialized)")
        register_index = track_gpio.find("honch_gpio_register")
        track_unlock_index = track_gpio.rfind("honch_gpio_lifecycle_unlock()")

        self.assertGreaterEqual(track_lock_index, 0)
        self.assertGreater(init_check_index, track_lock_index)
        self.assertGreater(register_index, init_check_index)
        self.assertGreater(track_unlock_index, register_index)

        shutdown_lock_index = shutdown_hook.find("honch_gpio_lifecycle_lock()")
        deinit_index = shutdown_hook.find("honch_gpio_deinit()")
        shutdown_unlock_index = shutdown_hook.rfind("honch_gpio_lifecycle_unlock()")

        self.assertGreaterEqual(shutdown_lock_index, 0)
        self.assertGreater(deinit_index, shutdown_lock_index)
        self.assertGreater(shutdown_unlock_index, deinit_index)


if __name__ == "__main__":
    unittest.main(verbosity=2)
