# Honch SDK Host Correctness Review TODO

Source: read-only host correctness / UX review
Branch referenced: `claude/sdk-analytics-integration-review-qJ8nO`
Scope: `core/`, `ports/{posix, esp-idf, arduino, micropython}`, `ports/react-native-relay`

## P0 - High

- [x] Make thread-safety mandatory where the platform can run concurrently.
  - Finding: every core lock is a no-op unless the platform supplies all four mutex ops.
  - Locations: `core/honch_lifecycle.c:12`, `core/honch_lifecycle.c:31`, `core/honch_lifecycle.c:50`, `core/platform.h:25-28`.
  - Impact: calling `track()` from one task and `tick()` from another without mutex ops can silently race shared queue and sequence state.
  - Action: fail init when mutex ops are absent on a multi-threaded platform.
  - Action: keep the all-or-nothing validation for mutex ops.

- [ ] Add an instance lock to the Arduino wrapper.
  - Finding: `begin()` and `shutdown()` mutate `_client` non-atomically.
  - Locations: `ports/arduino/src/Honch.cpp:85-153`.
  - Impact: concurrent `track()` can race shutdown and potentially use freed client state.
  - Action: guard all wrapper access to `_client` with an instance-level lock.
  - Action: document wrapper thread-safety expectations.

- [ ] Make the blocking `tick()` / `flush()` contract loud and enforceable.
  - Finding: transport `post_chunk` runs synchronous network I/O on the caller's thread.
  - Locations: `core/honch_internal.h:18`, `ports/esp-idf/esp_transport_http.c:357`, `ports/arduino/src/honch_arduino_transport.cpp:102-138`, `ports/micropython/mptransport_adapter.c:90`.
  - Impact: main loop, high-priority task, UI thread, MicroPython GIL, or ISR-adjacent callers can stall for seconds.
  - Action: document dedicated low-priority pump task usage as the expected integration pattern.
  - Action: explicitly warn against main loop, ISR, high-priority task, UI thread, and watchdog-sensitive paths.
  - Action: enforce a hard per-call wall-clock cap where each port can do so.

- [ ] Reject zero or unbounded MicroPython transport timeouts.
  - Finding: MicroPython timeout conversion can allow unbounded behavior.
  - Location: `ports/micropython/mptransport_adapter.c:35-41`.
  - Impact: `urequests.post` can hold the GIL without a reliable upper bound.
  - Action: reject `0` / unbounded timeout values and require a finite positive timeout.

- [ ] Route POSIX state writes through the configured durability mode.
  - Finding: `state_set` hardcodes `HONCH_DURABILITY_SYNC_ALWAYS` for state writes.
  - Locations: `ports/posix/posix_platform.c:368-380`, `ports/posix/posix_platform.c:619-624`.
  - Impact: `identify()` and firmware-version changes force file and directory `fsync`, causing flash wear and blocking I/O contention on eMMC / SD-backed embedded Linux.
  - Action: use `client->durability_mode` for state writes, matching the queue path.

## P1 - Medium

- [ ] Add optional allocator hooks to `platform_ops`.
  - Finding: the core uses libc `malloc` / `calloc` / `free` pervasively.
  - Location: `core/platform.h`.
  - Impact: hosts cannot confine analytics allocations to a custom heap, pool, or memory region.
  - Action: add optional `alloc` / `free` hooks with libc fallback.
  - Action: route client allocation, event payload allocation, RAM queue allocation, and flush scratch duplication through those hooks.

- [ ] Reduce per-flush allocation churn in queue policy.
  - Finding: every flush duplicates roughly six context strings and allocates each event from the RAM queue.
  - Locations: `core/honch_queue_policy.c:212-217`, `core/honch_ram_queue.c:153`.
  - Impact: steady-state variable-size churn can fragment a small shared heap.
  - Action: consider reusing snapshot buffers or moving repeated scratch into the client.

- [ ] Surface synchronous `honch_core_init` behavior to integrators.
  - Finding: init validates config, reads or derives device ID, reads and writes firmware-version state, reconciles queue storage, and queues `$device_boot` before returning.
  - Locations: `core/honch_core.c:1092-1132`.
  - Impact: init pays storage I/O latency and creates queue pressure even before the host calls `track()`.
  - Action: keep this behavior deliberate, but make it prominent in runtime contract docs and examples.

- [ ] Reduce per-flush HTTP / TLS object churn on Arduino.
  - Finding: Arduino creates and destroys `WiFiClientSecure` per flush and performs a fresh TLS handshake.
  - Location: `ports/arduino/src/honch_arduino_transport.cpp:107`.
  - Impact: heap churn, TLS allocation spikes, and handshake latency occur on the caller's thread.
  - Action: reuse the client where safe or document the cost and stack / heap requirements.

- [ ] Reduce HTTP client churn on ESP-IDF transport failures.
  - Finding: ESP-IDF reallocates URLs and tears down / reinitializes the HTTP client on transport error or status `0`, `408`, or `409`.
  - Location: `ports/esp-idf/esp_transport_http.c:401-410`.
  - Impact: heap churn and extra handshake latency alongside host code.
  - Action: avoid full client teardown unless required for correctness.

- [ ] Improve POSIX file-backed queue scaling.
  - Finding: file queue operations perform `opendir` / `readdir` / `qsort` per operation.
  - Locations: `ports/posix/posix_storage.c`.
  - Impact: syscall and heap churn scale poorly with queue depth on the host thread.
  - Action: cache ordering metadata or use a queue layout that avoids full directory scans per peek, batch read, and consume.

- [ ] Avoid process-wide `curl_global_init` as a hidden port side effect.
  - Finding: POSIX transport calls `curl_global_init`.
  - Location: `ports/posix/posix_transport_curl.c:16-21`.
  - Impact: can race with host-owned libcurl / OpenSSL initialization.
  - Action: document process-wide ownership clearly or require explicit host-managed curl initialization.

- [ ] Auto-stop native BLE scans when the relay is idle.
  - Finding: Android and iOS BLE scans do not auto-stop when idle.
  - Locations: `ports/react-native-relay/android/.../HonchReactNativeRelayModule.java:124-158`, `ports/react-native-relay/ios/HonchReactNativeRelay.m:62-73`.
  - Impact: continuous LE scanning drains battery and contends with host app BLE behavior.
  - Action: stop scans after an idle timeout and resume only when relay work requires it.

- [ ] Move relay frame queue writes off the JS thread or make them incremental.
  - Finding: each frame causes a full-store read, parse, and rewrite.
  - Locations: `ports/react-native-relay/src/relayQueue.ts:150-195`, `ports/react-native-relay/src/mmkvStore.ts:204-265`.
  - Impact: O(n) work per BLE notification can degrade host UI responsiveness during bursts.
  - Action: use append-oriented storage, batching, or native/background processing.

- [ ] Guarantee Android relay wake lock release on worker failures.
  - Finding: Android headless wake lock is only bounded by timeout on failure or unregistered-task paths.
  - Location: `ports/react-native-relay/android/.../HonchRelayUploadWorker.java:26-33`.
  - Impact: host app can keep a wake lock for up to 10 seconds after a failed relay path.
  - Action: ensure release in all failure and early-return paths.

- [ ] Stop requesting unnecessary Android fine location permission.
  - Finding: `requestRelayAndroidPermissions` always requests `ACCESS_FINE_LOCATION` despite `neverForLocation`.
  - Location: `ports/react-native-relay/src/permissions.ts:16-20`.
  - Impact: unexpected location prompt damages host app trust.
  - Action: request only required BLE permissions for the Android API level and manifest configuration.

## P2 - Low

- [ ] Guard test-only queue policy globals behind a test build flag.
  - Finding: `s_honch_test_max_wire_v2_encode_attempts` is a mutable static global written on the real multi-event flush path, and `honch_test_reset` / max helpers are exported non-static symbols.
  - Locations: `core/honch_queue_policy.c:41`, `core/honch_queue_policy.c:696-697`.
  - Impact: production namespace pollution and unsynchronized global writes across clients.
  - Action: hide these behind a test-only `#ifdef`.

- [ ] Make Arduino reference the shared core instead of vendoring copies.
  - Finding: Arduino vendors byte-identical copies of canonical `core/` sources.
  - Location: `ports/arduino/src/`.
  - Impact: drift risk versus other ports that compile `core/` by path.
  - Action: adjust Arduino packaging/build scripts to include the shared core sources directly.

- [ ] Make Arduino logging opt-in and respect log level.
  - Finding: Arduino log callback unconditionally writes to `Serial.println`.
  - Location: `ports/arduino/src/honch_arduino_platform.cpp:98`.
  - Impact: corrupts host applications that use Serial for their own protocol.
  - Action: disable Serial logging by default or require explicit callback configuration.
  - Action: respect configured log level.

- [ ] Remove or strongly gate Arduino insecure TLS mode.
  - Finding: `setInsecure()` path ships in the library.
  - Location: `ports/arduino/src/honch_arduino_transport.cpp:114`.
  - Impact: easy footgun that weakens endpoint validation.
  - Action: compile-gate, test-only gate, or require explicit dangerous opt-in.

- [ ] Review public symbol namespace exposure.
  - Finding: exported globals include `honch_init`, `honch_track`, `honch_flush`, `honch_tick`, `honch_now_millis`, `honch_random_hex`, and shared `TAG="honch"` log tags.
  - Impact: low collision risk due to `honch_` prefix, but still part of host namespace.
  - Action: keep prefix discipline and make internal helpers `static` where possible.

- [ ] Align package versions across ports.
  - Finding: Arduino `library.properties` reports `0.1.0` while CMake / ESP-IDF report `0.2.0`.
  - Impact: integrators can see inconsistent SDK versions.
  - Action: update all package metadata from one release source.

- [ ] Loosen React Native MMKV peer version constraints where compatible.
  - Finding: relay pins `react-native-mmkv` to `^4.3.1`.
  - Impact: host apps on MMKV v2 or v3 may hit dependency conflicts.
  - Action: test supported MMKV versions and widen peer dependency constraints where safe.

## Keep

- [ ] Keep the cooperative zero-thread model in core and every port.
- [ ] Keep crash-free error handling: no `abort`, `assert`, `exit`, or traps in core.
- [ ] Keep transport and OOM failures returning status codes.
- [ ] Keep core logging routed only through the platform log hook.
- [ ] Keep the core lock released during network I/O.
- [ ] Keep bounded mutex waits on ESP-IDF and Arduino.
- [ ] Keep bounded work per `tick` at one chunk.
- [ ] Keep the drop-oldest bounded queue on caller-provided buffers.
- [ ] Keep avoiding VLAs and `alloca`.
- [ ] Keep overflow-checked size math.
- [ ] Keep capped exponential backoff with jitter and `Retry-After` support.
- [ ] Keep relay import-time behavior side-effect-free.
- [ ] Keep relay permanent HTTP failures from hammering the capture service.
- [ ] Keep relay listener handles removable.

## Suggested Implementation Order

1. Thread-safety contract: make mutexes mandatory where required and add the Arduino instance lock.
2. Blocking I/O contract: document and enforce dedicated low-priority pump usage, and reject unbounded MicroPython timeouts.
3. Allocator hooks and POSIX state-write durability.
4. Relay scan auto-stop and off-JS-thread or incremental frame handling.
5. Quick hygiene: test-symbol `#ifdef`, de-vendor Arduino core, and remove unnecessary fine location permission.
