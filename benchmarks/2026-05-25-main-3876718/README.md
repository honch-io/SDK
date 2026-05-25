# Fresh main benchmark run

Run ID: `2026-05-25-main-3876718`

Artifacts for fresh benchmarks and footprints from merged main.

## Scope

- SDK: `main` at `3876718`
- Capture: `main` at `59e6362`
- Platform: `main` at `9f86dc9`
- Worker clean worktree: `origin/main` at `3724a03`
- Sandbox clean worktree: `origin/main` at `021226c`

## C core / POSIX

`c-core/posix-bench.csv` contains the fresh Release build microbenchmarks.

Key results:

- `init_shutdown`: 1035.12 us mean, 12014 SDK peak bytes, 139 max transport body bytes.
- `track_empty_properties`: 174.13 us mean.
- `track_small_properties`: 153.50 us mean.
- `track_nested_properties`: 261.58 us mean.
- `track_1kb_properties`: 163.32 us mean.
- `flush_1_success`: 165 us mean, 167 transport bytes.
- `flush_50_success`: 2090 us mean, 835 transport bytes, 695 max body bytes.
- `flush_200_success`: 12033 us mean, 2872 transport bytes, 695 max body bytes.

Size artifacts:

- `c-core/size.txt`
- `c-core/file-sizes.csv`
- `c-core/core-object-size.txt`
- `c-core/core-object-file-sizes.csv`
- `c-core/otool.txt`

Verification note: the full C test build currently fails under AppleClang because `core/test/test_state_consistency.c` trips `-Werror` on unused variables. The partial `ctest` output is captured in `c-core/ctest.txt`.

## ESP32

Footprint artifacts:

- `esp32/footprint-build-report.json`
- `esp32/footprint-monitor.log`
- `esp32/footprint-runtime-report.json`

Landing-page-safe ESP32 claims from `esp32/footprint-runtime-report.json`:

- Direct `libhonch.a` flash contribution: 32032 bytes, less than 32 KB.
- Direct `libhonch.a` static RAM contribution: 621 bytes, less than 1 KB.
- Runtime heap delta after `honch_init`: 4452 bytes, less than 8 KB.
- Representative `honch_track` CPU at 1 Hz: 0.0224%, less than 0.03%.

Benchmark artifacts:

- `esp32/benchtest-monitor.log`
- `esp32/benchtest-summary.json`
- `esp32/benchtest-monitor-partial.log` records the earlier failed `.95` host attempt.

ESP32 benchmark summary:

- Host: `http://192.168.1.122:8001`
- Device IP: `192.168.1.123`
- Wi-Fi RSSI: -65 dBm before, -63 dBm after.
- Events: 20, payload bytes per event: 64, flush every 10 events.
- Track latency: 710 us average, 548 us min, 1312 us max, 0 failures.
- Flush notify latency: 128682 us average, 276 us min, 208447 us max, 0 failures.
- HTTP payload chunks observed: 535 bytes and 503 bytes.
- End-of-run queued estimate: 0.

## Stack and platform

- `stack/capture.log`, `stack/proxy.log`, and `stack/worker.log` contain sandbox stack logs.
- Capture and proxy reached health checks during the run.
- Worker did not run cleanly because the current worker requests Google credentials even with Pub/Sub emulator environment variables set.
- `platform/dev-server-sessions.txt` records the local platform backend/frontend smoke run. Backend `/health` returned 200 and frontend served on `localhost:5173`.

## Pico W

Pico W artifacts:

- `pico-w/pico_w_footprint_bench.py`
- `pico-w/footprint-bench.log`
- `pico-w/footprint-bench-summary.json`

Pico W runtime context:

- MicroPython: 1.27.0
- Build: `RPI_PICO_W`
- Platform: `rp2`
- `_honch_core`: present in firmware
- Host: `http://192.168.1.122:8001`
- Device IP: `192.168.1.107`

Pico W footprint summary:

- Heap at boot marker: 7552 bytes allocated, 132544 bytes free.
- Heap after importing current wrapper and `_honch_core`: 13168 bytes allocated, 126928 bytes free.
- Import heap delta from boot marker: +5616 bytes allocated.
- Heap after `Honch` init: 13696 bytes allocated, 126400 bytes free.
- Init heap delta after import: +528 bytes allocated.
- Heap after 20 tracked events: 13728 bytes allocated, 126368 bytes free.
- Track heap delta after init: +32 bytes allocated.
- Heap after flush: 14448 bytes allocated, 125648 bytes free.
- Flush/end heap delta from boot marker: +6896 bytes allocated.

Pico W benchmark summary:

- Events: 20, payload bytes per event: 64.
- Track latency: 48467 us average, 18606 us min, 154747 us max, 0 failures.
- Flush latency: 2349252 us, error `none`.
- Shutdown latency: 279731 us, error `none`.

Note: Pico W results are runtime heap and timing measurements from the connected device. They do not include an isolated RP2040 firmware image map contribution for `_honch_core`.

## Pending

- RP2040 firmware image/map footprint isolation is still pending if marketing needs a flash-size claim for the MicroPython user C module itself.
