# Fault Capture Review Fix List

- [x] 1. Classify new benign ESP reset reasons correctly.
  - Files: `ports/esp-idf/honch/src/esp_platform.c`, `ports/arduino/src/honch_arduino_platform.cpp`.
  - Handle IDF 5.1+ reset reasons such as `ESP_RST_USB`, `ESP_RST_JTAG`, `ESP_RST_EFUSE`, and `ESP_RST_PWR_GLITCH` behind availability guards.
  - Treat USB/JTAG/EFUSE as non-fatal reset reasons. Decide and document whether power glitch maps to `brownout` or a distinct abnormal reason.
  - Add ESP-IDF and Arduino contract tests so these values cannot fall into fatal `unknown`.

- [x] 2. Make `HONCH_ENABLE_ERROR_TRACKING=OFF` testable.
  - Files: `core/test/test_state_consistency.c`, `ports/posix/test/test_honch.c`.
  - Guard `$error`-specific tests with `#if HONCH_ENABLE_ERROR_TRACKING`.
  - Keep boot reset-reason tests active after reset reason is decoupled from error tracking.
  - Verify `cmake -S ports/posix -B /tmp/... -DHONCH_ENABLE_ERROR_TRACKING=OFF && cmake --build ... && ctest ...`.

- [x] 3. Do not attach stale coredump summaries.
  - File: `ports/esp-idf/honch/src/esp_platform.c`.
  - Only attach ESP coredump summary fields for reset reasons that actually produce a fresh summary.
  - Omit `fault_pc`, `backtrace`, `task_name`, and `crash_summary_version` for unrelated watchdog/brownout/unknown resets.
  - Add coverage for symbolication-enabled brownout/watchdog without summary fields.

- [x] 4. Prevent re-init duplicate `$error` emission.
  - File: `ports/esp-idf/honch/src/esp_compat.c`.
  - Cache that the boot fault snapshot has already been consumed during this boot.
  - Subsequent `honch_shutdown()`/`honch_init()` cycles must not re-emit the same `$error`.
  - Add coverage for two init cycles after one panic snapshot yielding one `$error`.

- [ ] 5. Only set `crash_summary_version` when a summary exists.
  - File: `ports/esp-idf/honch/src/esp_platform.c`.
  - Make crash summary fill return whether summary retrieval succeeded.
  - Set `crash_summary_version=1` only when a relevant summary exists.
  - Decide whether `firmware_build_id` remains independent or is summary-scoped, then document it.

- [ ] 6. Preserve public `honch_config_t` positional compatibility.
  - File: `ports/esp-idf/honch/include/honch.h`.
  - Move `enable_error_tracking` and `enable_crash_symbolication` to the end of `honch_config_t`.
  - Add a contract test that pins field order or old positional initializer compatibility.

- [ ] 7. Decouple `$device_boot.reset_reason` from error tracking strip flag.
  - File: `core/src/honch_core.c` and vendored Arduino copy.
  - Keep `$error` emission behind `HONCH_ENABLE_ERROR_TRACKING`, but keep bounded boot reset reason handling available when error tracking is compiled out.
  - OFF builds should still emit supplied boot reset reasons.

- [ ] 8. Fix dead ESP-IDF dependency guard.
  - File: `ports/esp-idf/tests/test_sdk_contract.py`.
  - Parse `HONCH_ESP_REQUIRES` or make dependency assertions indentation-agnostic.
  - The test must fail if `esp_wifi` or other blocked unused dependencies are added.

- [ ] 9. Omit corrupted Xtensa backtraces.
  - File: `ports/esp-idf/honch/src/esp_platform.c`.
  - Check `core_summary.exc_bt_info.corrupted`; omit `backtrace` or emit an explicit bounded corruption flag.
  - Add coverage for corrupted summary handling.

- [ ] 10. Reduce duplicated reset mapping.
  - Files: ESP-IDF and Arduino reset mapping.
  - Short-term: keep both copies patched identically.
  - Later: centralize mapping or add parity tests so the two ports cannot drift.

## Decision Items

- [ ] Decide whether `CONFIG_HONCH_CRASH_SYMBOLICATION` should default to `n`.
- [ ] Decide whether `CONFIG_HONCH_ERROR_TRACKING` default `y` is acceptable while runtime tracking remains opt-in.
- [ ] Confirm whether `$error` enqueue failure should fail `honch_init()`.

## Verification Target

- [x] Normal POSIX CMake/ctest.
- [x] POSIX CMake/ctest with `-DHONCH_ENABLE_ERROR_TRACKING=OFF`.
- [ ] Python contract tests for ESP-IDF, Arduino, POSIX, and MicroPython.
- [x] ESP-IDF footprint build on Citadel.
- [ ] Optional ESP32 crash run for duplicate/stale summary behavior.
