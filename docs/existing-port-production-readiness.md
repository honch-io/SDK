# Existing Port Production Readiness Deep Dive

Review date: 2026-05-26

Scope: existing Honch SDK ports except React Native relay. Covered ports:

- ESP-IDF: `ports/esp-idf`
- Arduino ESP32: `ports/arduino`
- C/POSIX and embedded Linux foundation: `ports/posix`
- MicroPython: `ports/micropython`

This is a codebase-grounded improvement list, not a release sign-off. It is
based on the current tree, port docs, CI workflows, adapter code, tests, and
existing local production-readiness planning notes.

## Executive Summary

The canonical C core direction is the right foundation. The production gaps are
mostly around port validation, packaging, durable-storage semantics, runtime
evidence on real targets, and keeping testing hooks/build assumptions out of
release artifacts.

Current readiness by port:

| Port | Current confidence | Main blockers before calling it production-ready |
| --- | --- | --- |
| ESP-IDF | Medium-high | Real-device capture/offline/power-cycle evidence, configurable storage durability, memory/footprint gates, release packaging validation |
| Arduino ESP32 | Medium | CI and strict Arduino CLI compile exist; remaining blockers are no hardware E2E/power-cycle evidence and constrained-device runtime evidence |
| C/POSIX / embedded Linux | Medium-high as a dev/host SDK, medium for packaged embedded Linux | Install/CMake package exists; remaining blockers are no Yocto/Buildroot/RPi smoke path and curl transport not optimized |
| MicroPython | Medium | C-core user module, unix runtime CI smoke, and atomic queue/state writes exist; remaining blockers are no board runtime evidence and constrained-memory flush/read hardening |

Recommended order:

1. Run and record Arduino ESP32 hardware E2E/power-cycle evidence because host
   and compile gates now exist.
2. Add embedded Linux packaging examples around the POSIX port because install
   artifacts now exist but target consumption evidence is still thin.
3. Harden ESP-IDF real-device evidence, queue sizing, and footprint gates.
4. Run MicroPython board evidence before expanding board claims beyond unix
   runtime smoke coverage.

## Cross-Port Findings

### P0: Define a shared production-readiness gate

Each port should have a small `PRODUCTION_READINESS.md` or equivalent checklist
with current evidence, last verified date, supported targets, and known
limitations. React Native already has this pattern. The other ports rely on
README status text and scattered tests.

Required gate per port:

- Supported board/platform matrix.
- Build commands that must pass in CI.
- Unit/conformance coverage status.
- Capture E2E coverage status.
- Offline queue and retry coverage status.
- Power-cycle or restart persistence coverage status where the platform has
  persistence.
- Footprint or memory budget where the platform is constrained.
- Known limitations that block production wording.

### P0: Make compact chunk capture contract verification uniform

The ports are intended to upload `POST /capture` with
`application/vnd.honch.chunk`, `X-Honch-Project-Key`, and
`X-Honch-Stream-Id`. POSIX has strong host tests and an opt-in capture E2E.
ESP-IDF and Arduino have weaker evidence. MicroPython has wrapper/source-shape
tests plus an opt-in E2E that only runs when `_honch_core` exists.

Add a shared conformance runner that every C-derived port can use to prove:

- Same event semantics.
- Same reserved-property behavior.
- Same lifecycle behavior.
- Same wire-v2 framing.
- Same HTTP response handling for 202, 204, 401, 408, 409, 429, 4xx reject,
  and 5xx retry.

### P1: Keep production builds separate from test-instrumented builds

`ports/posix/CMakeLists.txt` now builds the production `honch_posix` target
without `HONCH_TESTING` and uses separate test/benchmark targets for hooks and
allocation instrumentation. Keep that split for future C-derived ports so fake
transport hooks and benchmark instrumentation never leak into release targets.

### P1: Keep duplicated vendored core copies guarded

Arduino vendors copied core headers and source files under
`ports/arduino/src`. ESP-IDF and POSIX use the repo root/shared core directly.
The copied Arduino core increases drift risk.

Options:

- Generate/sync Arduino vendored core files with a script and a CI guard that
  fails when they diverge from `core/`.
- Prefer a packaging layout where Arduino can include shared core files without
  manual duplication.

Minimum production guard is now present: `ports/arduino/scripts/check-core-sync.sh`
runs from Arduino verification and CI. Keep it required whenever `core/` or the
Arduino wrapper changes.

### P1: CI should fail when required platform verification is skipped

Arduino CI now installs `arduino-cli` and runs verification with
`--require-arduino-cli`; local developer runs may still skip compile checks when
the tool is absent. MicroPython CI now builds the unix port with `_honch_core`
and runs a runtime smoke test. Board hardware runtime remains a manual evidence
gap for both ports.

## ESP-IDF Port

Current state:

- Has an ESP-IDF component under `ports/esp-idf/honch`.
- CI builds the example for `esp32`, `esp32s3`, and `esp32c3`.
- Uses canonical core sources.
- Has RAM-first queue with NVS fallback.
- Uses `esp_http_client` and ESP certificate bundle.
- Has footprint/bench projects and Python migration/footprint tests.

### P0: Add real-device production E2E evidence

Current CI proves compile success, not runtime behavior. Before production
wording, run and record repeatable real-device tests:

- Boot event reaches capture.
- Custom event reaches capture.
- Identify and `$set_property` reach capture.
- Session start/end reaches capture.
- Connectivity event behavior is correct.
- Battery-low event behavior is correct with a fake battery callback.
- HTTP retry retains events until success.
- 401/auth rejection behavior does not spin forever.
- 4xx permanent rejection drops or dead-letters according to ESP policy.
- Power-cycle behavior is documented and tested for both RAM-first and NVS-only
  durability modes.

Recommended artifacts:

- `ports/esp-idf/PRODUCTION_READINESS.md`
- Device log transcript.
- Capture/ClickHouse verification transcript.
- Exact board, ESP-IDF version, target, and sdkconfig.

### P0: Make RAM-first vs NVS-only durability a public configuration

The README documents that strict reboot durability can be achieved by
"skipping the RAM queue setup and using the NVS queue operations directly", but
the public `honch_config_t` only accepts an event buffer and always goes through
the public init path.

Production users need an explicit config field, for example:

- `HONCH_ESP_QUEUE_RAM_FIRST`
- `HONCH_ESP_QUEUE_NVS_ONLY`

That avoids asking customers to depend on internal adapter details.

Acceptance criteria:

- Public config supports selecting queue durability.
- README documents latency/durability tradeoffs.
- Tests or examples prove both modes.
- Real-device power-cycle test covers NVS-only mode.

### P1: Make queue sizing configurable

NVS fallback depth is hard-coded to 256. RAM queue capacity is derived from the
caller buffer and capped by internal constants. Production customers will need
to tune queue depth for flash budget, expected offline windows, and event rate.

Add:

- Max queued events config.
- Max event bytes config or documented fixed limit.
- NVS namespace/partition guidance.
- Behavior when queue is full: drop oldest vs return queue full vs reject new
  event.

### P1: Avoid large per-read NVS scratch allocations

NVS reader reads the full blob into a heap scratch buffer before copying the
requested offset window. On small ESP32 variants this can create avoidable heap
pressure during flush.

Improve by:

- Adding batch read support for ESP-IDF storage, or
- Reading directly into caller buffers when possible, or
- Bounding and documenting maximum event/blob size for ESP.

Add a heap watermark test during flush with representative payload sizes.

### P1: Add footprint and heap gates to CI

The repo has footprint tooling, but CI only builds the example. Production
readiness should include regression gates for:

- Flash size.
- Static RAM.
- Minimum free heap after init.
- Minimum free heap after queueing N events.
- Minimum free heap during flush.

Use the existing footprint project and turn current report data into thresholds.

### P1: Add ESP-IDF version matrix or stated version support

The README says ESP-IDF >= 5.0. CI currently uses `espressif/idf:v5.3`.

Either:

- Test the supported minimum and current recommended version, or
- Narrow the documented support range to the tested version.

Suggested matrix:

- ESP-IDF 5.0 or 5.1 if still claimed.
- ESP-IDF 5.3 current CI.
- Optional latest stable image once release cadence is established.

### P2: Harden release packaging checks

The component manager packaging model depends on the repo-root package layout.
Existing migration tests look for the root manifest, but production release
should also verify:

- `idf.py add-dependency "honch-io/honch^x.y.z"` works from a clean app.
- Published component includes `core/` sources.
- Examples do not depend on local untracked sdkconfig/build state.
- Version in README, manifest, and code stays in sync.

### P2: Improve public error/status observability

The ESP public API returns a small `honch_err_t`, while the core has richer
statuses such as rate-limited/server/rejected. Production users need enough
diagnostic detail without parsing logs.

Add:

- `honch_status_string()` equivalent for ESP public errors.
- Last transport status or last HTTP status accessor.
- Optional SDK log callback or structured log sink.

## Arduino ESP32 Port

Current state:

- Arduino wrapper around the canonical core.
- ESP32-only.
- Host wrapper test exists.
- CI installs `arduino-cli` and requires ESP32 example compile checks.
- PlatformIO metadata and an ESP32 example project are present and covered by
  CI.
- Scheduled flushing is cooperative through `Honch.tick()` / `Honch.loop()`;
  no hidden background task is started.
- TLS root CA configuration is application-owned; `insecureSkipTlsVerify` exists.

### Done: Add Arduino CI that installs and requires `arduino-cli`

Arduino CI now installs `arduino-cli`, installs the ESP32 Arduino core, runs
`verify-arduino.sh --require-arduino-cli`, checks the vendored core sync, and
builds the PlatformIO example. The remaining production blocker is hardware
runtime evidence, not host compile coverage.

### P0: Run real ESP32 Arduino capture E2E

Current docs say it has not been end-to-end tested on ESP32 hardware.

Required tests:

- Wi-Fi connect, init, track, flush.
- Offline queue survives restart using `Preferences`.
- Failed upload keeps queued events.
- Successful upload consumes queued events.
- 401 and permanent 4xx behavior matches the core contract.
- HTTPS works with a real trust configuration, not just `setInsecure`.

### Done: Fix TLS/root CA production story

Arduino exposes `rootCaPem`, examples use a CA PEM placeholder with
`insecureSkipTlsVerify = false`, and tests cover the TLS config path.
`insecureSkipTlsVerify` remains available for local testing only.

### P1: Make storage more configurable and observable

Arduino storage uses `Preferences` with a fixed queue depth of 256 and no public
namespace/depth config. It also persists every queued event to NVS, which has
latency and flash-wear implications.

Add:

- Queue depth config.
- Namespace config or documented fixed namespace.
- RAM-first option if latency matters.
- Public queue depth accessor.
- Tests for queue overflow/drop behavior.

### P1: Guard against core drift

Arduino carries copied core source/header files. Add a sync script and CI guard:

- Compare `ports/arduino/src/honch_*` core copies with `core/src`.
- Compare `ports/arduino/src/honch/core/*` with `core/include/honch/core/*`.
- Fail CI if copies drift.

### P2: Expand public API ergonomics carefully

The current API uses the typed `honch_value_t`/`honch_property_t` property
surface shared with the portable C core. Before production release, decide
whether Arduino needs additional ergonomic helpers:

- Keep the typed C-style property API for v0.1 and document validation/failure
  behavior.
- Or add a tiny helper for common scalar values without pulling in heavy
  dependencies.

Do not add a large Arduino DSL until the storage/TLS/runtime evidence is solid.

## C/POSIX And Embedded Linux Port

Current state:

- Strongest host-side test coverage.
- Uses canonical core directly.
- Has persistent filesystem queue and state.
- Has libcurl transport.
- Has examples and benchmark harness.
- Has opt-in real capture E2E.
- CI builds/tests on Ubuntu.

### Done: Add install/package outputs

C/POSIX now installs the library and headers and exports a CMake package
consumable with `find_package(honch_posix REQUIRED)`. Remaining packaging work
is target-platform evidence: external consumer CI, optional `pkg-config`, and
Yocto/Buildroot/Raspberry Pi examples.

### Done: Keep `HONCH_TESTING` out of production target

The production `honch_posix` target no longer receives `HONCH_TESTING`; tests
and benchmarks use separate instrumented targets.

### P1: Add embedded Linux packaging targets

The POSIX README says this covers embedded Linux-style systems, but there is no
Yocto, Buildroot, or Raspberry Pi packaging path.

Add:

- Yocto `.bb` recipe or documented layer snippet.
- Buildroot package definition or external tree example.
- Raspberry Pi smoke app instructions.
- Cross-compile toolchain file example.

Minimum production evidence:

- Cross-compile CI using an ARM Linux toolchain.
- Run host tests on x86_64.
- Build example for ARM.

### P1: Add macOS CI or narrow POSIX support claim

README mentions macOS/Linux development harnesses. CI only runs Ubuntu.

Add:

- macOS build/test matrix, or
- State Linux as the production-supported target and macOS as best-effort.

### P1: Optimize libcurl transport for production flush paths

`posix_transport_curl.c` creates a new curl easy handle and header list for
every flush. Existing benchmark notes already identify curl-handle/header reuse
as a production cleanup.

Add:

- Per-client reusable curl easy handle.
- Reusable header list where safe.
- Cleanup on shutdown.
- Tests that transport still handles project key and stream id correctly.

### P1: Add filesystem corruption and disk-full tests

POSIX has good queue tests, but production devices will see partial writes,
read-only filesystems, full disks, unexpected directories, and power loss.

Add tests for:

- `.tmp` file cleanup on startup.
- Non-regular files in queue/state dirs.
- Queue file larger than max event bytes.
- Rename failure.
- Fsync failure.
- Disk full / `ENOSPC`.
- Permission denied.

### P2: Add logging and diagnostics

Current POSIX platform log adapter is a no-op. Production users need
diagnostics for queued events, retry delay, HTTP status, and storage failures.

Add:

- Optional log callback in public config.
- Last flush status/accessor.
- Queue depth accessor if not already public.
- Example logs in README.

## MicroPython Port

Current state:

- Python package is now a thin wrapper over `_honch_core`.
- C user module includes canonical core sources.
- Host tests validate wrapper behavior and source shape.
- README correctly says full runtime validation requires building MicroPython
  with the user module.
- CI now builds the MicroPython unix port with `_honch_core` and runs a runtime
  smoke test.
- Queue and state writes use temp-file plus rename, and storage startup removes
  orphaned `.tmp` files.
- `transport_timeout_ms` is passed to `urequests.post(timeout=...)` as a
  rounded-up seconds value.
- README explicitly scopes this port to MicroPython and says CircuitPython is
  not covered by the current user C module/native ABI path.
- Scheduled flushing is cooperative through `client.tick()`; there is no
  background flush thread in the C-core-derived binding.

Optional hardware CI/manual evidence remains for:

- Raspberry Pi Pico W / `rp2`.
- ESP32 MicroPython.

### P1: Avoid loading whole queue files repeatedly during flush

The MicroPython storage reader loads an entire queued file into a Python bytes
object for peek/read. On constrained boards this can cause avoidable memory
pressure.

Improvements:

- Implement `queue_read_batch` with bounded memory, or
- Implement chunked file reads for the reader path, or
- Lower and document `max_event_bytes` defaults for MicroPython.

Add memory-focused smoke tests on a constrained board before production claims.

### P1: Replace weak/fallback randomness with explicit entropy policy

The platform adapter tries `urandom.getrandbits`, then falls back to
`random.getrandbits`. That fallback may not be cryptographically strong and
can produce weak device IDs/session IDs depending on board/runtime.

Do one of:

- Require `urandom` for generated identities and return an error otherwise.
- Allow caller-provided `device_id` and document entropy limitations.
- Provide board-specific random adapter hooks in C.

### P1: Improve exception mapping from C to Python

`honch_micropython_raise_status` raises generic `RuntimeError` with a string,
and the Python wrapper maps by message when no `.status` attribute exists. That
is brittle.

Improve by:

- Exposing typed `_honch_core` exceptions, or
- Raising an exception object with a `status` attribute, or
- Returning status codes to the Python layer and mapping there.

### P2: Add mip/package verification

The package has `package.json`, but `mip` only installs wrapper files and
requires firmware with `_honch_core`.

Add tests/docs for:

- `mip` install result.
- Import failure message when `_honch_core` is absent.
- Frozen manifest path.
- Board firmware build command.

## Suggested Milestones

### Milestone 1: Production-readiness docs and CI guardrails

- Add `PRODUCTION_READINESS.md` to ESP-IDF, Arduino, POSIX, and MicroPython.
- Keep Arduino CI strict with `arduino-cli`.
- Keep POSIX production/test target split guarded.
- Keep Arduino core drift check required.
- Keep MicroPython unix runtime build/test CI required.

### Milestone 2: Runtime evidence

- Run ESP-IDF real-device capture/offline/power-cycle test.
- Run Arduino ESP32 real-device capture/offline/power-cycle test.
- Run POSIX local capture E2E and record evidence.
- Build MicroPython unix port with `_honch_core` and run runtime tests.

### Milestone 3: Packaging

- POSIX external consumer/package examples and optional `pkg-config`.
- Yocto/Buildroot/RPi examples.
- ESP-IDF component manager clean-app verification.
- MicroPython `mip`/frozen firmware verification.

### Milestone 4: Durability and constrained-device hardening

- ESP-IDF queue sizing and footprint thresholds.
- Arduino queue sizing and hardware scheduler evidence.
- MicroPython bounded reads.
- Heap/footprint thresholds for ESP-IDF, Arduino, and MicroPython board builds.

## Concrete Next Tickets

No selected tickets are pending from the current batch.
