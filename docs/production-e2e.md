# Production E2E

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove whether the canonical C core, POSIX SDK, ESP-IDF SDK, Arduino ESP32 SDK, and MicroPython SDK are production-ready on the compact chunk wire path through repeatable host tests, real capture E2E tests, and real-device ESP32 testing. Assess the React Native relay package separately because it has its own mobile relay runtime, storage, BLE, and scheduling surface.

**Architecture:** Run the campaign in layers: static/unit/conformance first, local capture E2E second, then real-device ESP32 and optional MicroPython-board tests. Treat Arduino as its own ESP32 wrapper surface because it vendors the canonical C core and has Arduino-specific transport, storage, TLS, and cooperative scheduler behavior. Treat the React Native relay separately because direct SDK readiness does not prove mobile relay BLE, storage, scheduling, or E2E behavior.

**Tech Stack:** C11, CMake, CTest, POSIX/pthreads/libcurl/zlib, ESP-IDF, FreeRTOS, ESP32 over serial, MicroPython-compatible Python package, CPython host tests, `mpremote`, Bun, TypeScript, Vitest, local Honch sandbox/capture services, ClickHouse.

---

## Current Repo Assessment

- C core and POSIX are implemented and have CTest coverage under `core/test` and `ports/posix/test`. POSIX now also has a gated real-capture E2E target at `ports/posix/test/test_e2e_capture.c`.
- ESP-IDF is converted onto the canonical C core and has example, benchtest, footprint, static migration tests, and real-board benchmark hooks.
- Arduino ESP32 is a wrapper around a vendored copy of the canonical C core. It has host wrapper tests, core drift checks, Arduino CLI and PlatformIO compile paths, TLS root CA configuration, and cooperative scheduled flushing through `Honch.tick()` / `Honch.loop()`.
- MicroPython is now a wrapper around the `_honch_core` user C module for production behavior. It has wrapper/shape tests, conformance checks, examples, package metadata, and a gated real-capture E2E test that only runs when `_honch_core` is available.
- The canonical SDK ingest path is compact chunk wire to `POST /capture` with aliases `/e` and `/chunks`, `Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and `X-Honch-Stream-Id`. Old public knobs and old capture routes are not part of this campaign except as negative migration guards.
- React Native relay is a separate package. It has TypeScript relay frame decoding, durable-store interfaces, in-memory test storage, upload retry/backoff orchestration, native module interface scaffolding, Android/iOS skeletons, and an example app harness. Its uploader now posts compact wire-v2 chunks to `POST /capture`, but it still needs production mobile BLE/storage/scheduling implementations and real mobile E2E evidence before relay readiness can be claimed.
- The open review findings in `local-docs/review-wip.md` still matter for production readiness. Passing this test plan does not erase unresolved review findings; it tells us which are blocking in practice.

## Execution Rules

- Do not start this campaign unless explicitly instructed.
- Prefer the automated runner for repeat release checks:

```bash
python3 tools/release_e2e.py --profile release --continue-on-fail
```

The runner writes a timestamped evidence directory under
`/private/tmp/honch-production-e2e-*`, including per-step logs,
`summary.json`, and `PRODUCTION_READINESS_REPORT.md`. Use `--profile smoke` for
the fastest deterministic gate, `--profile host` for the broader deterministic
local gate, `--profile services` for sandbox/capture E2E, `--profile
toolchains` for ESP-IDF/Arduino/PlatformIO compile gates, `--profile hardware`
for configured board preflights, and `--profile full` for release plus
hardware. Required missing services, toolchains, or hardware inputs are
reported as `BLOCKED`; they are not counted as a pass.
- Before execution, run `git status --short` and preserve all user-owned changes.
- Use a fresh terminal transcript/log directory for the campaign, for example `/private/tmp/honch-production-e2e-YYYYMMDD-HHMMSS`.
- Do not commit generated logs, build directories, sdkconfig local secrets, `.pyc`, `node_modules`, or device output.
- Keep Arduino CLI data and build output on the X9 Pro drive:
  `HONCH_ARDUINO_HOME=/Volumes/X9 Pro/Arduino` and
  `HONCH_ARDUINO_BUILD_ROOT=/Volumes/X9 Pro/honch-arduino-verify`.
  Do not install the Arduino ESP32 core, downloads, or compile cache onto the
  local drive.
- Real-device steps require explicit confirmation of serial port, Wi-Fi SSID, Wi-Fi password, capture endpoint reachable from the device, and API token.
- When sandbox/capture comes from the sibling capture repo, record that repo's branch and commit before E2E execution. The campaign expects a capture build that accepts compact chunk wire on `/capture`; a capture build that only serves legacy `/batch` is not a valid target for canonical SDK sign-off.
- Any failure gets recorded with the exact command, exit code, relevant log excerpt, and suspected owner: test bug, product bug, environment issue, or missing feature.

## Task 1: Preflight And Repo Hygiene

**Files:**
- Read: `README.md`
- Read: `.github/workflows/posix.yml`
- Read: `.github/workflows/esp-idf.yml`
- Read: `.github/workflows/arduino.yml`
- Read: `.github/workflows/micropython.yml`
- Read: `local-docs/review-wip.md`
- Output: `/private/tmp/honch-production-e2e-*/00-preflight.txt`

- [ ] **Step 1: Capture branch and worktree state**

Run:

```bash
git branch --show-current
git status --short
git log -5 --oneline
```

Expected:

```text
Branch is known.
No unexpected tracked modifications.
Recent commits include the bugfix commits being evaluated.
```

- [ ] **Step 2: Record recent remediation commits and stale review entries**

Run:

```bash
git log -10 --oneline
rg -n "Open Finding|Arduino|arduino|durability|package version|response handling|scheduled flush" local-docs/review-wip.md docs/production-e2e.md
```

Expected:

```text
Recent fix commits are visible. Any review entries still describing already-fixed Arduino response handling, Arduino scheduled flush defaults, POSIX package version, ESP-IDF durability docs, or whitespace-only issues are recorded as stale plan/review debt before execution.
```

- [ ] **Step 3: Record unresolved review risks**

Run:

```bash
sed -n '1,80p' local-docs/review-wip.md
```

Expected:

```text
Remaining hitlist is recorded before test execution.
```

## Task 2: Host C Core And POSIX Quality Gates

**Files:**
- Read: `core/CMakeLists.txt`
- Read: `ports/posix/CMakeLists.txt`
- Test: `core/test/test_storage_contract.c`
- Test: `core/test/test_packetizer.c`
- Test: `core/test/test_retry.c`
- Test: `ports/posix/test/test_honch.c`
- Test: `ports/posix/test/test_sdk_contract.py`
- Test: `ports/posix/test/test_install_package.py`
- Output: `/private/tmp/honch-production-e2e-*/10-posix-host.txt`

- [ ] **Step 1: Configure clean POSIX Debug build**

Run:

```bash
cmake -S ports/posix -B /private/tmp/honch-prod-posix-debug \
  -DHONCH_BUILD_TESTS=ON \
  -DHONCH_BUILD_EXAMPLES=ON \
  -DCMAKE_BUILD_TYPE=Debug
```

Expected:

```text
Configure succeeds.
```

- [ ] **Step 2: Build Debug targets**

Run:

```bash
cmake --build /private/tmp/honch-prod-posix-debug
```

Expected:

```text
All C core, POSIX, test, and example targets build without warnings-as-errors failures.
```

- [ ] **Step 3: Run Debug CTest suite**

Run:

```bash
ctest --test-dir /private/tmp/honch-prod-posix-debug --output-on-failure
```

Expected:

```text
100% tests passed.
```

- [ ] **Step 4: Configure ASan/UBSan build**

Run:

```bash
cmake -S ports/posix -B /private/tmp/honch-prod-posix-asan \
  -DHONCH_BUILD_TESTS=ON \
  -DHONCH_BUILD_EXAMPLES=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

Expected:

```text
Configure succeeds.
```

- [ ] **Step 5: Build and run sanitizer suite**

Run:

```bash
cmake --build /private/tmp/honch-prod-posix-asan
ctest --test-dir /private/tmp/honch-prod-posix-asan --output-on-failure
```

Expected:

```text
100% tests passed.
No AddressSanitizer or UndefinedBehaviorSanitizer reports.
```

- [ ] **Step 6: Run SDK contract guards directly**

Run:

```bash
python3 ports/posix/test/test_sdk_contract.py
```

Expected:

```text
All POSIX SDK contract tests pass, confirming `/capture` compact chunk transport is the default and legacy public knobs/routes are absent from the POSIX SDK.
```

- [ ] **Step 7: Build Release benchmark target**

Run:

```bash
cmake -S ports/posix -B /private/tmp/honch-prod-posix-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DHONCH_BUILD_TESTS=OFF \
  -DHONCH_BUILD_EXAMPLES=OFF \
  -DHONCH_BUILD_BENCHMARKS=ON
cmake --build /private/tmp/honch-prod-posix-release --target honch_posix_bench
```

Expected:

```text
Release benchmark target builds.
```

- [ ] **Step 8: Verify POSIX install package and version metadata**

Run:

```bash
python3 -m unittest ports.posix.test.test_install_package
cmake -S ports/posix -B /private/tmp/honch-prod-posix-install \
  -DHONCH_BUILD_TESTS=OFF \
  -DHONCH_BUILD_EXAMPLES=OFF \
  -DHONCH_BUILD_BENCHMARKS=OFF
cmake --build /private/tmp/honch-prod-posix-install
cmake --install /private/tmp/honch-prod-posix-install \
  --prefix /private/tmp/honch-prod-posix-prefix
rg -n 'PACKAGE_VERSION "0\.2\.0"' \
  /private/tmp/honch-prod-posix-prefix/lib/cmake/honch_posix/honch_posixConfigVersion.cmake
```

Expected:

```text
The installed CMake package exports headers, library, package config files, and version 0.2.0, matching the documented C/POSIX SDK version.
```

## Task 3: Local Capture E2E For POSIX And MicroPython

**Files:**
- Read: `ports/micropython/tests/test_e2e_capture.py`
- Read: `ports/posix/test/test_e2e_capture.c`
- Read: `ports/posix/README.md`
- Read: `../capture/README.md` if the sibling capture repo is present
- Output: `/private/tmp/honch-production-e2e-*/20-local-capture.txt`

- [ ] **Step 1: Start or verify local Honch sandbox**

Run the existing project-approved sandbox command:

```bash
./honch --plain sandbox
```

Expected:

```text
Sandbox reports capture, worker, and ClickHouse services ready.
```

- [ ] **Step 2: Verify local service health from host**

Run:

```bash
curl -i --max-time 5 http://127.0.0.1:8001/health
curl -i --max-time 5 http://127.0.0.1:8080/
curl -i --max-time 5 http://127.0.0.1:8123/
```

Expected:

```text
HTTP endpoints respond within timeout.
```

- [ ] **Step 3: Record capture repo contract**

Run if `../capture` exists:

```bash
git -C ../capture branch --show-current
git -C ../capture log -1 --oneline
rg -n '"/capture"|"/e"|"/chunks"|application/vnd.honch.chunk|X-Honch-Project-Key|X-Honch-Stream-Id|POST /batch|application/cbor' \
  ../capture/README.md ../capture/src ../capture/tests
```

Expected:

```text
Capture branch/commit is recorded. Capture exposes the compact chunk `/capture` path and does not require legacy `/batch` CBOR for canonical SDK E2E.
```

- [ ] **Step 4: Build POSIX real-capture E2E target**

Run:

```bash
cmake -S ports/posix -B /private/tmp/honch-prod-posix-e2e \
  -DHONCH_BUILD_TESTS=ON \
  -DHONCH_BUILD_E2E=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /private/tmp/honch-prod-posix-e2e --target honch_posix_e2e
```

Expected:

```text
The POSIX E2E target builds against the canonical C core and libcurl transport.
```

- [ ] **Step 5: Run POSIX real-capture E2E**

Run:

```bash
HONCH_E2E=1 \
HONCH_E2E_ENDPOINT=http://127.0.0.1:8001 \
HONCH_E2E_CAPTURE_HEALTH_URL=http://127.0.0.1:8001/health \
HONCH_E2E_WORKER_HEALTH_URL=http://127.0.0.1:8080/ \
HONCH_E2E_CLICKHOUSE_URL=http://127.0.0.1:8123 \
HONCH_E2E_TOKEN=honch_e2e_test_key \
HONCH_E2E_PROJECT_ID=00000000-0000-0000-0000-000000000002 \
HONCH_E2E_CLICKHOUSE_DATABASE=platform \
ctest --test-dir /private/tmp/honch-prod-posix-e2e --output-on-failure -R honch_posix_e2e
```

Expected:

```text
The POSIX SDK sends compact chunk wire frames through capture, verifies ClickHouse rows, preserves identity across restart, resets identity, and passes.
```

- [ ] **Step 6: Run MicroPython real-capture host E2E**

Run:

```bash
HONCH_E2E=1 \
HONCH_E2E_ENDPOINT=http://127.0.0.1:8001 \
HONCH_E2E_CAPTURE_HEALTH_URL=http://127.0.0.1:8001/health \
HONCH_E2E_WORKER_HEALTH_URL=http://127.0.0.1:8080/ \
HONCH_E2E_CLICKHOUSE_URL=http://127.0.0.1:8123 \
HONCH_E2E_TOKEN=honch_e2e_test_key \
HONCH_E2E_PROJECT_ID=00000000-0000-0000-0000-000000000002 \
HONCH_E2E_CLICKHOUSE_DATABASE=platform \
PYTHONPATH=ports/micropython \
python3 -m unittest -v ports.micropython.tests.test_e2e_capture
```

Expected:

```text
The E2E test queues, flushes, verifies ClickHouse rows, persists identity across restart, resets identity, and passes.
```

- [ ] **Step 7: Record canonical SDK transport evidence**

Run:

```bash
rg -n '"/capture"|application/vnd.honch.chunk|X-Honch-Project-Key|X-Honch-Stream-Id|post_chunk' \
  core ports/posix ports/esp-idf ports/arduino ports/micropython
```

Expected:

```text
All production SDK transports use the compact chunk capture path. Any `application/cbor`, `POST /batch`, or `post_batch` match outside the React Native relay package is recorded as a blocker.
```

## Task 4: ESP-IDF Host Builds And Static Gates

**Files:**
- Read: `ports/esp-idf/README.md`
- Read: `ports/esp-idf/example/main/app_main.c`
- Read: `ports/esp-idf/benchtest/main/app_main.c`
- Read: `ports/esp-idf/footprint/README.md`
- Test: `ports/esp-idf/tests/test_sdk_contract.py`
- Test: `ports/esp-idf/tests/test_footprint_measurement.py`
- Test: `ports/esp-idf/tests/test_durability_config.py`
- Output: `/private/tmp/honch-production-e2e-*/30-esp-host-builds.txt`

- [ ] **Step 1: Run ESP SDK contract static tests**

Run:

```bash
python3 ports/esp-idf/tests/test_sdk_contract.py
python3 ports/esp-idf/tests/test_footprint_measurement.py
python3 ports/esp-idf/tests/test_durability_config.py
```

Expected:

```text
All ESP static, footprint parser, and durability config tests pass, confirming `/capture` compact chunk transport is the default, legacy public knobs/routes are absent from the ESP-IDF SDK, and public durability mode is implemented and documented.
```

- [ ] **Step 2: Build ESP example for ESP32**

Run from the X9 Pro ESP-IDF v6.0.1 shell:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/example set-target esp32
idf.py -C ports/esp-idf/example build
```

Expected:

```text
Example firmware builds for esp32.
```

- [ ] **Step 3: Build ESP benchtest for ESP32**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/benchtest set-target esp32
idf.py -C ports/esp-idf/benchtest build
```

Expected:

```text
Benchtest firmware builds for esp32.
```

- [ ] **Step 4: Build ESP footprint variants**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
./tools/measure_esp_idf_footprint.py --target esp32
```

Expected:

```text
Footprint report is generated and current direct libhonch.a flash/static-RAM claims remain inside documented thresholds.
```

- [ ] **Step 5: Build target matrix when toolchain supports it**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/example set-target esp32s3
idf.py -C ports/esp-idf/example build
idf.py -C ports/esp-idf/example set-target esp32c3
idf.py -C ports/esp-idf/example build
```

Expected:

```text
Example builds for esp32s3 and esp32c3, matching CI intent. Any target-specific failure is recorded as a production gap.
```

## Task 5: ESP32 Real-Device E2E Against Local Capture

**Files:**
- Read: `ports/esp-idf/example/main/Kconfig.projbuild`
- Read: `ports/esp-idf/benchtest/main/Kconfig.projbuild`
- Output: `/private/tmp/honch-production-e2e-*/40-esp32-device.txt`

- [ ] **Step 1: Confirm device and network inputs**

Record these values before flashing:

```text
Serial port: /dev/cu.usbserial-0001
ESP target: esp32
Wi-Fi SSID: <provided by user at execution time>
Wi-Fi password: <provided by user at execution time>
Capture host reachable from ESP32: http://<LAN-IP>:8001
API token: honch_e2e_test_key or provided project token
```

Expected:

```text
ESP32, host, and capture service are on the same network, and host firewall allows inbound ESP32 access to port 8001.
```

- [ ] **Step 2: Verify capture health from LAN address**

Run:

```bash
curl -i --max-time 5 http://<LAN-IP>:8001/health
```

Expected:

```text
HTTP 2xx/3xx health response from the same URL configured into ESP firmware.
```

- [ ] **Step 3: Configure benchtest without committing secrets**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/benchtest menuconfig
```

Set:

```text
Wi-Fi SSID
Wi-Fi Password
Honch API Key
Honch Host = http://<LAN-IP>:8001
Bench event count = production smoke value, for example 100
Flush every = 10
Repeat = disabled for first run
```

Expected:

```text
Local sdkconfig is generated or updated but remains ignored by git.
```

- [ ] **Step 4: Erase flash, flash benchtest, and monitor**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/benchtest -p /dev/cu.usbserial-0001 erase-flash flash monitor
```

Expected monitor markers:

```text
Connected to Wi-Fi
HTTP readiness confirmed
Honch initialized
BENCH_RUN_START
BENCH_TRACK_SUMMARY failures=0
BENCH_FLUSH_SUMMARY failures=0
BENCH_RESOURCE_SUMMARY label=after
BENCH_RUN_END
```

- [ ] **Step 5: Verify ESP events reached ClickHouse**

Run:

```bash
curl --max-time 5 "http://127.0.0.1:8123/?query=SELECT%20event%2C%20count()%20FROM%20platform.events%20WHERE%20event%20%3D%20'bench_event'%20GROUP%20BY%20event%20FORMAT%20TabSeparated"
```

Expected:

```text
bench_event count is greater than or equal to the bench event count.
```

- [ ] **Step 6: Exercise GPIO real-device path**

Run while monitor is active:

```text
Press the BOOT button at least 10 times during or after benchtest.
```

Expected:

```text
No crash, no watchdog reset, no GPIO worker teardown error, and bench_button or button_pressed events appear in capture after flush.
```

- [ ] **Step 7: Power-cycle persistence test**

Procedure:

```text
1. Let benchtest enqueue and flush at least one batch.
2. Press reset or power-cycle ESP32.
3. Let it boot and send another batch.
4. Query ClickHouse for device_id values for bench_event from this run.
```

Expected:

```text
Device identity persists across reboot unless reset/erase-flash is intentionally performed.
```

- [ ] **Step 8: Offline queue preservation test**

Procedure:

```text
1. Stop the local capture service or block ESP32 access to port 8001.
2. Run benchtest long enough to queue events and observe flush failures.
3. Restore capture service.
4. Trigger or wait for flush.
5. Query ClickHouse for delayed bench_event delivery.
```

Expected:

```text
Retryable transport failures preserve pending events and later delivery succeeds without duplicate overcount beyond expected retry behavior.
```

- [ ] **Step 9: Shutdown and teardown stability**

Procedure:

```text
1. Use the monitor logs to confirm no panic or abort after the benchmark completes.
2. Reset the board three times.
3. Repeat BOOT-button presses after each boot.
```

Expected:

```text
No FreeRTOS queue/task crashes, no WDT resets, no memory exhaustion trend across repeated boots.
```

## Task 6: ESP32 Footprint Runtime Measurement

**Files:**
- Read: `ports/esp-idf/footprint/README.md`
- Tool: `tools/measure_esp_idf_footprint.py`
- Output: `/private/tmp/honch-production-e2e-*/50-esp32-footprint.txt`

- [ ] **Step 1: Build and flash Honch footprint app**

Run:

```bash
export IDF_TOOLS_PATH="/Volumes/X9 Pro/Espressif/tools"
source "/Volumes/X9 Pro/Espressif/esp-idf-v6.0.1/export.sh"
idf.py -C ports/esp-idf/footprint \
  -B build-footprint-esp32-honch \
  -D IDF_TARGET=esp32 \
  -D HONCH_FOOTPRINT_WITH_SDK=ON \
  -p /dev/cu.usbserial-0001 \
  erase-flash flash monitor
```

Expected monitor markers:

```text
HONCH_FOOTPRINT_RUNTIME phase=before_honch
HONCH_FOOTPRINT_RUNTIME phase=after_honch_init
HONCH_FOOTPRINT_RUNTIME phase=after_api_setup
HONCH_FOOTPRINT_CPU
```

- [ ] **Step 2: Merge monitor markers into footprint report**

Run:

```bash
./tools/measure_esp_idf_footprint.py \
  --target esp32 \
  --skip-build \
  --monitor-log ports/esp-idf/footprint/footprint-monitor.log
```

Expected:

```text
Report contains runtime heap and CPU values, and claims remain within documented limits or regressions are recorded.
```

## Task 7: Arduino ESP32 Host, Compile, And Device Gates

**Files:**
- Read: `ports/arduino/README.md`
- Read: `ports/arduino/scripts/verify-arduino.sh`
- Read: `ports/arduino/scripts/check-core-sync.sh`
- Read: `ports/arduino/examples/HonchBasic/HonchBasic.ino`
- Read: `ports/arduino/examples/HonchOfflineQueue/HonchOfflineQueue.ino`
- Test: `ports/arduino/test/host/test_arduino_wrapper.cpp`
- Test: `ports/arduino/test/test_ci_workflow.py`
- Test: `ports/arduino/test/test_tls_config.py`
- Output: `/private/tmp/honch-production-e2e-*/60-arduino-esp32.txt`

- [ ] **Step 1: Run Arduino source-shape tests**

Run:

```bash
python3 -m unittest ports.arduino.test.test_ci_workflow ports.arduino.test.test_tls_config
```

Expected:

```text
Arduino CI, PlatformIO manifest, TLS root CA, and example source-shape tests pass.
```

- [ ] **Step 2: Verify vendored core sync**

Run:

```bash
ports/arduino/scripts/check-core-sync.sh
```

Expected:

```text
Arduino vendored core sources and headers match the canonical C core.
```

- [ ] **Step 3: Run host wrapper behavior test**

Run:

```bash
cmake -S ports/arduino/test/host -B /private/tmp/honch-prod-arduino-host
cmake --build /private/tmp/honch-prod-arduino-host
/private/tmp/honch-prod-arduino-host/honch_arduino_wrapper_test
```

Expected:

```text
Host wrapper test passes, including Arduino HTTP response policy and cooperative scheduled flush defaults.
```

- [ ] **Step 4: Compile Arduino examples with strict Arduino CLI mode**

Run with Arduino CLI installed and indexes available:

```bash
HONCH_ARDUINO_HOME="/Volumes/X9 Pro/Arduino" \
HONCH_ARDUINO_BUILD_ROOT="/Volumes/X9 Pro/honch-arduino-verify" \
  ports/arduino/scripts/verify-arduino.sh --require-arduino-cli
```

Expected:

```text
Host wrapper test passes and HonchBasic/HonchOfflineQueue compile for the supported ESP32 FQBN matrix.
```

- [ ] **Step 5: Compile PlatformIO example**

Run with PlatformIO installed:

```bash
pio run -d ports/arduino/examples/platformio
```

Expected:

```text
The PlatformIO HonchBasic example builds for esp32dev.
```

- [ ] **Step 6: Arduino real-device capture smoke**

Procedure:

```text
1. Confirm ESP32 board, serial port, Wi-Fi SSID/password, LAN capture URL, API token, and TLS mode.
2. Flash HonchBasic with host set to http://<LAN-IP>:8001 for local capture, or with a real root CA PEM for HTTPS capture.
3. Let the sketch call Honch.tick() long enough to exercise interval flush.
4. Query ClickHouse for boot/default event rows with sdk_platform=arduino-esp32.
```

Expected:

```text
Arduino ESP32 sends compact chunk wire frames to capture, response handling matches the wire spec, scheduled flush works without manual Honch.flush(), and events appear in ClickHouse.
```

- [ ] **Step 7: Arduino offline/retry smoke**

Procedure:

```text
1. Flash HonchOfflineQueue or block the capture endpoint after initialization.
2. Queue events while capture is unavailable.
3. Restore capture service.
4. Let Honch.tick() trigger retry/flush.
5. Query ClickHouse for delayed event delivery and inspect serial logs for errors.
```

Expected:

```text
Retryable transport failures preserve pending events and later delivery succeeds without false success on non-202/204 responses.
```

## Task 8: MicroPython Host Production Gates

**Files:**
- Read: `ports/micropython/README.md`
- Read: `ports/micropython/AGENTS.md`
- Test: `ports/micropython/tests/test_sdk.py`
- Test: `ports/micropython/tests/test_conformance.py`
- Test: `ports/micropython/tests/test_e2e_capture.py`
- Output: `/private/tmp/honch-production-e2e-*/70-micropython-host.txt`

- [ ] **Step 1: Run compileall for SDK, tests, and examples**

Run:

```bash
cd ports/micropython
python3 -m compileall honch tests examples
```

Expected:

```text
All modules compile under host Python.
```

- [ ] **Step 2: Run host unit and conformance tests**

Run:

```bash
PYTHONPATH=ports/micropython python3 -m unittest discover -v \
  -s ports/micropython/tests -t .
```

Expected:

```text
All host MicroPython SDK tests pass.
```

- [ ] **Step 3: Run real-capture host E2E**

Run the MicroPython command from Task 3 Step 6.

Expected:

```text
MicroPython package sends compact chunk wire frames through `_honch_core` to capture, and observed ClickHouse rows match identity, lifecycle, session, reset, and auto-property expectations.
```

- [ ] **Step 4: Check packaging metadata**

Run:

```bash
cd ports/micropython
python3 - <<'PY'
import json
from pathlib import Path
pkg = json.loads(Path("package.json").read_text())
missing = [dst for _, dst in pkg["urls"] if not Path(dst).exists()]
if missing:
    raise SystemExit("missing package files: " + ", ".join(missing))
print("package file list ok")
PY
```

Expected:

```text
package file list ok
```

## Task 9: MicroPython Real-Board E2E If Hardware Exists

**Files:**
- Read: `ports/micropython/examples/pico_w_main.py`
- Read: `ports/micropython/examples/basic.py`
- Output: `/private/tmp/honch-production-e2e-*/80-micropython-board.txt`

- [ ] **Step 1: Confirm board availability**

Record:

```text
Board type: Pico W, ESP32 MicroPython, or other MicroPython target
Serial path: <provided at execution time>
MicroPython version: captured from REPL
Network: same LAN as capture endpoint, if using Wi-Fi
```

Expected:

```text
Board can run MicroPython, mount/copy files with mpremote, and reach capture endpoint.
```

- [ ] **Step 2: Install package onto board**

Run:

```bash
mpremote connect auto fs mkdir :honch
mpremote connect auto fs cp -r ports/micropython/honch :honch
```

Expected:

```text
Package files are present on device filesystem.
```

- [ ] **Step 3: Deploy a local-capture example**

Edit a temporary copy of `ports/micropython/examples/pico_w_main.py` outside the repo, setting:

```text
WIFI_SSID
WIFI_PASSWORD
ENDPOINT_URL = http://<LAN-IP>:8001
API_KEY = honch_e2e_test_key
```

Run:

```bash
mpremote connect auto fs cp /private/tmp/honch-pico-w-main.py :main.py
mpremote connect auto reset
mpremote connect auto repl
```

Expected:

```text
Board boots, imports honch, connects to Wi-Fi, queues events, flushes, and prints no unhandled exception.
```

- [ ] **Step 4: Verify board events in ClickHouse**

Run:

```bash
curl --max-time 5 "http://127.0.0.1:8123/?query=SELECT%20event%2C%20device_model%2C%20sdk_platform%20FROM%20platform.events%20WHERE%20sdk_platform%20%3D%20'micropython'%20ORDER%20BY%20received_at%20DESC%20LIMIT%2010%20FORMAT%20TabSeparated"
```

Expected:

```text
Rows show MicroPython board-originated events with sdk_platform=micropython.
```

- [ ] **Step 5: Power-cycle persistence test**

Procedure:

```text
1. Capture initial device_id from logs or event row.
2. Power-cycle the board.
3. Send another event.
4. Query latest rows for the device model and compare device_id.
```

Expected:

```text
Device identity persists across board restart.
```

## Task 10: React Native Relay Assessment And Gates

**Files:**
- Read: `ports/react-native-relay/README.md`
- Read: `ports/react-native-relay/package.json`
- Read: `ports/react-native-relay/src/frame.ts`
- Read: `ports/react-native-relay/src/relayQueue.ts`
- Read: `ports/react-native-relay/src/uploader.ts`
- Test: `ports/react-native-relay/test/frame.test.ts`
- Test: `ports/react-native-relay/test/relayQueue.test.ts`
- Test: `ports/react-native-relay/test/uploader.test.ts`
- Output: `/private/tmp/honch-production-e2e-*/90-react-native-relay.txt`

- [ ] **Step 1: Typecheck and test relay package**

Run:

```bash
cd ports/react-native-relay
bun run typecheck
bun run test
```

Expected:

```text
Typecheck and Vitest pass.
```

- [ ] **Step 2: Assess production feature gaps**

Record these findings from source inspection:

```text
Frame decode exists.
In-memory and durable-interface chunk assembly exists.
Upload function exists.
Tests exist for frame decode, queue assembly, upload headers, retry/drain behavior, native interfaces, package shape, and the example harness.
BLE receiver interface exists; platform BLE/GATT implementation is still stubbed.
Durable storage interface exists; production AsyncStorage/SQLite/MMKV adapter does not.
Retry/backoff policy exists.
Background upload scheduler interface exists; platform workers are not production-complete.
Native module/package scaffolding exists.
Example app harness exists.
No end-to-end relay from ESP32 packetizer through mobile to capture exists.
Relay upload targets compact wire-v2 chunks at `POST /capture`, matching the canonical capture contract.
```

Expected:

```text
Relay is classified as prototype unless all missing production capabilities are implemented and real mobile E2E evidence proves the active capture service path.
```

- [ ] **Step 3: Define relay production acceptance criteria**

Acceptance criteria before relay can be called production-ready:

```text
1. Decide whether relay should keep a separate relay-envelope capture endpoint or migrate to the compact chunk capture contract.
2. Durable storage adapter for React Native AsyncStorage, SQLite, MMKV, or a project-approved storage layer.
3. BLE frame receiver integration or a test harness that feeds real device frames exactly as mobile would receive them.
4. Reassembly handles out-of-order chunks, duplicates, corrupted frames, missing first/final chunks, and app restart mid-message.
5. Upload retry policy preserves messages on retryable failures and removes only after accepted capture response.
6. E2E test sends packetizer frames from firmware or a byte-accurate C packetizer fixture into relay queue, uploads to capture, and verifies ClickHouse.
7. Public API documented for mobile apps.
```

Expected:

```text
Any missing acceptance criterion is filed as explicit relay work, not hidden inside the SDK production sign-off.
```

## Task 11: Cross-SDK Conformance And Wire Compatibility

**Files:**
- Read: `spec/wire-format-v2.md`
- Read: `spec/auto-properties.md`
- Read: `spec/relay-envelope.md`
- Read: `spec/relay-chunks.md`
- Read: `spec/conformance/`
- Output: `/private/tmp/honch-production-e2e-*/100-conformance.txt`

- [ ] **Step 1: Validate conformance fixture coverage**

Run:

```bash
find spec/conformance -maxdepth 4 -type f -name '*.json' -print | sort
```

Expected:

```text
Fixtures cover event shape, auto-stamp conflict handling, lifecycle boot, session, identity reset, envelope, HTTP response policy, and compact wire-v2 frames.
```

- [ ] **Step 2: Compare current SDK test coverage to fixtures**

Run:

```bash
rg -n "basic-track|auto_stamp|boot_event|session-track|identity-reset|response-policy|basic_batch|wire-v2|application/vnd.honch.chunk" core ports spec
```

Expected:

```text
Each production SDK either has direct fixture tests or a recorded gap.
```

- [ ] **Step 3: Verify compact chunk compatibility**

Run:

```bash
python3 spec/conformance/test_wire_v2_fixtures.py
rg -n "honch_core_build_wire_v2_message|honch_core_post_wire_v2_message|post_chunk|application/vnd.honch.chunk" \
  core ports spec
```

Expected:

```text
Core encoder, production SDK transports, conformance fixtures, and capture parser agree on compact chunk wire frames, status handling, and HTTP headers.
```

- [ ] **Step 4: Verify relay frame compatibility separately**

Run:

```bash
rg -n "HONCH_PACKETIZER_HEADER_SIZE|decodeRelayFrame|relay frame|payloadLength|sequence|offset" core ports/react-native-relay spec/relay-chunks.md
```

Expected:

```text
C packetizer frame layout, TypeScript decoder, and spec agree on version, source, flags, sequence, offset, payload length, reserved bytes, and CRC handling. Any relay upload contract mismatch with capture is recorded under React Native relay, not under canonical SDK readiness.
```

## Task 12: Final Production Readiness Report

**Files:**
- Create at execution time: `/private/tmp/honch-production-e2e-*/PRODUCTION_READINESS_REPORT.md`

- [ ] **Step 1: Summarize pass/fail status by surface**

Report sections:

```text
C core / POSIX:
ESP-IDF host builds:
ESP32 real-device:
ESP-IDF footprint:
Arduino ESP32 host/compile:
Arduino ESP32 real-device:
MicroPython host:
MicroPython real board:
React Native relay:
Cross-SDK conformance:
Open review findings:
```

Expected:

```text
Each section has PASS, FAIL, BLOCKED, or NOT PRODUCTION READY with evidence.
```

- [ ] **Step 2: Classify blockers**

Use these categories:

```text
P0: data loss, crash, memory corruption, security/secret leak, false successful delivery
P1: production feature missing, flaky real-device behavior, unsupported documented platform
P2: docs/test coverage gap, non-blocking reliability issue, benchmark/footprint regression
```

Expected:

```text
Every failure has severity, owner area, reproduction command, and next action.
```

- [ ] **Step 3: Make a release recommendation**

Use one of:

```text
Release-ready for C/POSIX only.
Release-ready for ESP-IDF only.
Release-ready for Arduino ESP32 only.
Release-ready for MicroPython only.
Release-ready for C/POSIX + ESP-IDF + Arduino ESP32 + MicroPython.
Not release-ready.
Relay is prototype-only.
```

Expected:

```text
Recommendation is evidence-backed and does not overstate untested real-device or relay behavior.
```

## Known Likely Outcomes Before Execution

- C core/POSIX: likely close, but unresolved review findings still need either fixes or explicit risk acceptance.
- ESP-IDF: host builds are known to pass recently; production confidence requires real ESP32 capture, power-cycle, offline queue, GPIO, and footprint runs.
- Arduino ESP32: host wrapper and packaging checks exist; production confidence requires Arduino CLI/PlatformIO compile evidence plus real ESP32 capture, scheduled-flush, offline/retry, TLS, and power-cycle runs.
- MicroPython: implementation exists and has meaningful host tests plus a gated local-capture E2E. Real MicroPython hardware status is unverified until `mpremote` board tests run.
- React Native relay: not production-ready today. It now targets the active compact chunk capture contract, but it still needs production native BLE/scheduling/storage implementations and a real mobile E2E.
