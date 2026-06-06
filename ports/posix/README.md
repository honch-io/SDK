# Honch C/POSIX SDK

Stable C/POSIX SDK for Honch analytics on connected hardware and embedded Linux-style systems.

This package is intended for:

- macOS/Linux development harnesses
- embedded Linux devices
- reusable C core validation before the same behavior is adapted by ESP-IDF,
  MicroPython, and future C-derived ports

The shared cross-SDK contract lives in [`../../spec/`](../../spec/). C/POSIX sends the same compact chunk wire payloads as the ESP-IDF SDK.

## Status

Stable `0.2.0`.

## Current Capabilities

Implemented:

- C11 SDK library
- persistent local queue before network delivery
- persistent generated `device_id`
- persistent current `distinct_id`
- epoch milliseconds event timestamps
- compact chunk wire uploads to `POST /capture`
- cooperative `honch_tick()` flushing with retry backoff and jitter
- persisted firmware version change detection
- `$set_property` event API parity with the ESP-IDF SDK
- battery level auto-stamping and low-battery lifecycle events
- automatic core lifecycle events for boot and shutdown
- explicit event tracking and flushing
- reset behavior for factory-reset-style identity rotation
- deterministic C tests using fake transport hooks
- minimal POSIX example
- connected camera usage example
- POSIX GPIO edge-tracking adapter example

## Layout

```text
include/                Public POSIX SDK headers
src/                    POSIX adapters and public compatibility layer
../../core/             Canonical portable C core
test/                   C test executables
example/posix_device/   Minimal smoke example
example/connected_camera/
example/posix_gpio/     Platform-adapter pattern for GPIO edge events
bench/                  POSIX benchmark harness
```

## Build

Requirements:

- CMake 3.20+
- C11 compiler
- libcurl

From this directory:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The C targets build with warnings enabled and warnings treated as errors.

## Install

Install the production library and headers:

```sh
cmake -S . -B build-install -DHONCH_BUILD_TESTS=OFF -DHONCH_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/honch-posix
cmake --build build-install --target honch_posix
cmake --install build-install
```

The install exports a CMake package:

```cmake
find_package(honch_posix REQUIRED)
target_link_libraries(app PRIVATE honch::honch_posix)
```

## Examples

Run the minimal example:

```sh
./build/example/posix_device/honch_posix_example
```

Run the connected camera example:

```sh
./build/example/connected_camera/honch_connected_camera_example
```

Run the GPIO adapter example:

```sh
./build/example/posix_gpio/honch_posix_gpio_example
```

The examples use the configured `endpoint_url` and are intended to run against a
real capture endpoint or an explicit local development service.

## Public API

Header:

```text
include/honch/honch.h
```

API:

```c
honch_status_t honch_init(honch_client_t **client, const honch_config_t *config);
honch_status_t honch_track(honch_client_t *client, const char *event_name, const honch_property_t *properties, size_t property_count);
honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const honch_property_t *traits, size_t trait_count);
honch_status_t honch_set_property(honch_client_t *client, const char *key, honch_value_t value);
honch_status_t honch_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_session_end(honch_client_t *client);
honch_status_t honch_flush(honch_client_t *client);
honch_status_t honch_reset(honch_client_t *client);
honch_status_t honch_shutdown(honch_client_t *client);
const char *honch_get_device_id(honch_client_t *client);
honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
const char *honch_status_string(honch_status_t status);
```

`honch_status_t` is the canonical C/POSIX status type. For ESP-IDF source
compatibility, the public header also exposes `honch_err_t` as an alias and
defines `HONCH_ERR_*` names that map to the corresponding portable
`HONCH_ERROR_*` statuses. ESP-IDF's NVS-specific storage error maps to
`HONCH_ERROR_IO` in C/POSIX.

Required config:

- `api_key`
- `endpoint_url`
- `device_model`
- `firmware_version`
- `queue_directory`

Optional config:

- `device_id`: generated and persisted when omitted
- `environment`: defaults to `production`
- `batch_size`: defaults to `20`, capped at `50`
- `max_queued_events`
- `max_event_bytes`
- `transport_timeout_ms`
- `flush_interval_seconds`: defaults to `60`
- `flush_min_interval_ms`: defaults to `10000`; use `HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS` for benchmarks
- `flush_event_threshold`: defaults to `30`
- `flush_retry_initial_ms`: defaults to `1000`
- `flush_retry_max_ms`: defaults to `300000`
- `battery_callback`: returns `0`-`100`, or negative when unknown
- `battery_low_threshold`: defaults to `15`
- `auto_properties_callback`: optional platform adapter hook for automatic event properties
- `auto_properties_userdata`: caller-owned context passed to `auto_properties_callback`
- `connectivity_callback`: optional fast callback; return `0` while offline or the radio is off
- `connectivity_userdata`: caller-owned context passed to `connectivity_callback`
- `durability_mode`: defaults to `HONCH_DURABILITY_OS_BUFFERED`, which skips
  per-event fsyncs for lower enqueue latency and reduced flash wear; set
  `HONCH_DURABILITY_SYNC_ALWAYS` when power-loss durability is required

`honch_get_device_id` returns a borrowed pointer for single-threaded callers.
The returned pointer is owned by the SDK and remains valid until `honch_reset`
or `honch_shutdown`. Use `honch_copy_device_id` when another thread may reset or
shut down the client while the ID is being read.

honch init does synchronous work on the caller's thread. It validates config,
creates and reconciles queue directories, loads or persists identity and
firmware-version state, and queues `$device_boot` before returning. It does not
perform network I/O; delivery remains cooperative through `honch_tick()` or
explicit flush calls.

`honch_shutdown` returns `HONCH_ERROR_NOT_INITIALIZED` when called without a
client, matching the ESP-IDF SDK's shutdown-before-init behavior. For a valid
client, it queues `$device_shutdown`, attempts a synchronous shutdown flush,
frees the client, and returns the first shutdown error encountered or
`HONCH_OK`.

`honch_session_start` starts an in-memory analytics session, queues a
`$session_start` event, and attaches the generated `$session_id` to later
events until `honch_session_end` queues `$session_end`.

Auto-stamped property keys such as `$device_id`, `$session_id`, and
`$sdk_platform` are owned by the SDK. User-supplied properties using those keys
are rejected before queueing so application input cannot spoof SDK context.

`honch_set_property` queues a `$set_property` event whose properties contain the
provided key/value pair. It does not persist context onto future events.

Public property inputs use typed `honch_value_t` values that map directly onto
wire-format-v2 tags. Strings and keys are validated as UTF-8, object/map keys
must be unique, numeric values must fit their target wire type, and non-finite
floating point values return `HONCH_ERROR_INVALID_ARGUMENT`. C/POSIX fails
closed instead of silently dropping invalid user properties.

When `battery_callback` is configured, valid readings are stamped as
`$battery_level`. The SDK queues `$battery_low` once when the level drops below
`battery_low_threshold`, then arms it again after the level recovers.

`auto_properties_callback` lets platform adapters add automatic event
properties without putting platform-specific code in the reusable core. The
callback receives a typed sink and can add typed values:

```c
static honch_status_t add_platform_properties(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    int rssi = *(int *)userdata;
    return sink(sink_ctx, "$wifi_rssi", honch_i64(rssi));
}
```

The SDK validates each key and typed value before appending it. Adapter
properties are collected separately and only approved adapter-owned keys are
accepted. Core-owned values such as `$device_id`, `$sdk_platform`, and
`$firmware_version` cannot be overridden by either user input or an adapter. The
callback should return quickly because event enqueueing waits for it to finish.

Call `honch_tick()` periodically from your main loop or scheduler to perform
bounded cooperative flush work. When `flush_interval_seconds` or
`flush_event_threshold` are zero, the SDK uses defaults of 60 seconds and 30
queued events. Successful outbound uploads are spaced by
`flush_min_interval_ms`, which defaults to 10000 ms. Retryable transport
failures use exponential backoff with jitter. Shutdown always attempts a
synchronous best-effort flush for a valid client.

honch_tick() may block for up to the configured transport timeout because the
HTTP POST is synchronous and runs on the caller's thread. Do not call
honch_tick() from a latency-sensitive control loop, GPIO edge path, camera frame
path, UI thread, or watchdog-sensitive section. If the host has work with
tighter latency requirements, pump Honch from a dedicated low-priority thread:
Each scheduled tick posts at most one wire chunk, so large queued uploads may
need several pump iterations to finish.

```c
#include <pthread.h>
#include <unistd.h>

static void *honch_pump_thread(void *arg)
{
    honch_client_t *client = (honch_client_t *)arg;
    for (;;) {
        honch_tick(client);
        usleep(250000);
    }
    return NULL;
}

static void start_honch_pump(honch_client_t *client)
{
    pthread_t thread;
    pthread_create(&thread, NULL, honch_pump_thread, client);
    pthread_detach(thread);
}
```

Do not call `honch_tick()` while connectivity is unavailable. If your scheduler
cannot guarantee that, provide `connectivity_callback`; offline ticks keep the
flush pending, and explicit `honch_flush()` returns `HONCH_ERROR_OFFLINE`
without DNS/TLS work or retry backoff growth.

Flushes use the compact wire encoder in the shared core and send chunk frames to
`POST /capture` with `Content-Type: application/vnd.honch.chunk`. The project
key is sent as `X-Honch-Project-Key`; a boot-scoped stream ID is sent as
`X-Honch-Stream-Id`.

Use HTTPS in production. Local HTTP should only be used for intentional local
Capture testing.

The POSIX transport performs process-wide libcurl initialization by calling
`curl_global_init(CURL_GLOBAL_DEFAULT)` once through `pthread_once`. The SDK
cleans up each client-owned easy handle but does not call curl_global_cleanup;
hosts that manage libcurl directly should account for that process-wide libcurl
lifecycle.

GPIO tracking is intentionally kept out of the reusable C core. Use a platform
adapter, like `example/posix_gpio`, to debounce platform-specific GPIO edge
signals and translate accepted edges into normal `honch_track` calls with at
least a `{"pin": <number>}` property.

## Basic Usage

```c
#include "honch/honch.h"

int main(void)
{
    honch_config_t config = {
        .api_key = "local-dev-key",
        .endpoint_url = "http://127.0.0.1:8765",
        .device_id = NULL,
        .device_model = "ActionCam X1",
        .firmware_version = "1.2.3",
        .environment = "dev",
        .queue_directory = ".honch-queue",
        .batch_size = 10,
        .max_queued_events = 100,
        .max_event_bytes = 8192,
        .transport_timeout_ms = 10000,
        .flush_interval_seconds = 60,
        .flush_min_interval_ms = 10000,
        .flush_event_threshold = 30,
        .battery_callback = NULL,
        .battery_low_threshold = 15,
        .durability_mode = HONCH_DURABILITY_DEFAULT
    };

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        return 1;
    }

    const honch_property_t traits[] = {
        honch_prop("plan", honch_str("beta"))
    };
    honch_identify(client, "user-123", traits, 1);

    honch_session_start(client, "recording");

    const honch_property_t properties[] = {
        honch_prop("mode", honch_str("hdr")),
        honch_prop("resolution", honch_str("4k"))
    };
    honch_track(client, "recording_started", properties, 2);
    honch_session_end(client);
    honch_flush(client);

    honch_shutdown(client);
    return 0;
}
```

## Local Storage

The SDK stores state and queued events under `queue_directory`.

```text
.honch-queue/
  pending/                 queued event files waiting to flush
  dead/                    permanently rejected event files
  state/
    device_id              generated or configured device identity
    distinct_id            current analytics identity
    firmware_version        last-seen firmware version for update detection
```

Queue behavior:

- a queue directory is intended to be owned by one active SDK client/process at a time
- events are written atomically through temp-file rename
- `HONCH_DURABILITY_OS_BUFFERED` keeps atomic rename behavior but skips
  per-write fsyncs by default; this lowers enqueue/state-update latency and
  flash wear, but queued events and state changes may be lost after OS crash or
  power loss before storage is flushed
- `HONCH_DURABILITY_SYNC_ALWAYS` fsyncs each queued event or state file and its
  directory before returning from the write operation
- events are stored as `.hqe` files
- startup removes temporary write files
- queue length is bounded by `max_queued_events`
- flush requests are sent in batches capped at 50 events
- when full, the oldest event is dropped before accepting a new event
- retryable failures keep files in `pending/`
- permanent rejections move attempted files to `dead/`
- `honch_reset` rotates identity state and clears pending/dead queues as a
  factory-reset boundary

Sharing the same `queue_directory` across concurrent clients can delay flush
visibility and may temporarily exceed `max_queued_events`, because each client
maintains an in-memory queue count cache between disk reconciliations.

The POSIX file-backed queue scans and sorts queue files for ordered read/peek
paths. It is intended for bounded embedded-style queues, not very large offline
backlogs. Keep `max_queued_events` conservative for the target filesystem, or
provide a custom queue when large offline backlogs are expected.

The default portable RAM queue used by embedded ports is bounded and compact:
consuming a non-tail event uses an O(n) memmove per consumed event to keep the
caller-provided buffer contiguous. The POSIX port uses file-backed storage by
default, but custom RAM-backed queues should keep that consume cost in mind when
raising queue limits.

## Test Coverage

Current C tests cover:

- init validation
- event persistence
- epoch milliseconds timestamp encoding
- compact chunk wire transport and response handling
- chunk wire transport and response handling
- strict typed-value validation for public property input
- generated `device_id` persistence
- configured and generated device ID access
- `$set_property` event emission
- auto-stamped property conflict handling
- session start/end events and `$session_id` event context
- boot and shutdown lifecycle events
- firmware update detection
- battery callback, `$battery_level`, and `$battery_low`
- identify payload and persisted `distinct_id`
- bounded queue drop-oldest behavior
- retryable flush preserving pending events
- background threshold flush and retry backoff
- multi-event flush
- permanent rejection dead-letter behavior
- reset queue clearing
- reset identity behavior
- opt-in real capture E2E flush

Run:

```sh
ctest --test-dir build --output-on-failure
```
