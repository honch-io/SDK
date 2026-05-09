# Honch C/POSIX SDK

C/POSIX SDK foundation for Honch analytics on connected hardware and embedded Linux-style systems.

This package is intended for:

- macOS/Linux development harnesses
- embedded Linux devices
- future reusable C core work for Zephyr, Arduino, bare-metal C, and MicroPython bindings

The shared cross-SDK contract lives in [`../spec/`](../spec/). This package should stay aligned with that contract, while keeping encoder and transport boundaries isolated so it can move to the planned CBOR ingest format when the org-level API/spec update is ready.

## Current Status

Implemented:

- C11 SDK library
- persistent local queue before network delivery
- persistent generated `device_id`
- persistent current `distinct_id`
- ISO-8601 UTC event timestamps
- tokenized batch flush envelopes
- gzip-compressed JSON flush requests
- isolated batch encoder boundary for the future CBOR transition
- default background flushing with retry backoff and jitter
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

Temporary development behavior:

- the current transport implementation is a development harness, not the final CBOR ingest contract

Do not optimize around the current JSON ingest API as the long-term contract. The package is expected to move to CBOR when the shared ingest API/spec update lands.

## Layout

```text
honch/include/          Public C headers
honch/src/              SDK implementation and internal modules
test/                   C test executables
example/posix_device/   Minimal smoke example
example/connected_camera/
example/posix_gpio/     Platform-adapter pattern for GPIO edge events
```

## Build

Requirements:

- CMake 3.20+
- C11 compiler
- libcurl
- zlib

From this directory:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The C targets build with warnings enabled and warnings treated as errors.

## E2E Capture Test

The real ingest E2E test is opt-in because it sends events to the configured
service. It includes local-stack defaults for the Honch development E2E stack,
and every value can be overridden with environment variables.

```sh
cmake -S . -B build-e2e -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_E2E=ON
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure -R honch_c_core_e2e
```

Default local E2E settings:

- `HONCH_E2E_ENDPOINT`: `http://127.0.0.1:8001`
- `HONCH_E2E_CAPTURE_HEALTH_URL`: `http://127.0.0.1:8001/health`
- `HONCH_E2E_WORKER_HEALTH_URL`: `http://127.0.0.1:8080/`
- `HONCH_E2E_TOKEN`: `honch_e2e_test_key`
- `HONCH_E2E_PROJECT_ID`: `00000000-0000-0000-0000-000000000002`
- `HONCH_E2E_CLICKHOUSE_URL`: `http://127.0.0.1:8123`
- `HONCH_E2E_CLICKHOUSE_DATABASE`: `platform`

The test verifies capture health, worker health, ClickHouse reachability, SDK
validation failures, batching, lifecycle events, identify, user properties,
reserved-property protection, auto properties, battery-low telemetry, sessions,
device ID persistence across restart, reset identity rotation, and ingested
ClickHouse rows.

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
honch/include/honch/honch.h
```

API:

```c
honch_status_t honch_init(honch_client_t **client, const honch_config_t *config);
honch_status_t honch_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_set_property(honch_client_t *client, const char *key, const char *value_json);
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
- `flush_event_threshold`: defaults to `30`
- `flush_retry_initial_ms`: defaults to `1000`
- `flush_retry_max_ms`: defaults to `300000`
- `disable_background_flush`: set nonzero to disable the background worker
- `battery_callback`: returns `0`-`100`, or negative when unknown
- `battery_low_threshold`: defaults to `15`
- `auto_properties_callback`: optional platform adapter hook for automatic event properties
- `auto_properties_userdata`: caller-owned context passed to `auto_properties_callback`

`honch_get_device_id` returns a borrowed pointer for single-threaded callers.
The returned pointer is owned by the SDK and remains valid until `honch_reset`
or `honch_shutdown`. Use `honch_copy_device_id` when another thread may reset or
shut down the client while the ID is being read.

`honch_shutdown` returns `HONCH_ERROR_NOT_INITIALIZED` when called without a
client, matching the ESP-IDF SDK's shutdown-before-init behavior. For a valid
client, it queues `$device_shutdown`, attempts a synchronous shutdown flush,
frees the client, and returns the first shutdown error encountered or
`HONCH_OK`.

`honch_session_start` starts an in-memory analytics session, queues a
`$session_start` event, and attaches the generated `$session_id` to later
events until `honch_session_end` queues `$session_end`.

Auto-stamped property keys such as `$device_id`, `$session_id`, and
`$sdk_platform` are owned by the SDK. Per-event properties using those keys are
ignored so the SDK-stamped values win.

`honch_set_property` queues a `$set_property` event whose properties contain the
provided key/value pair. It does not persist context onto future events.

JSON-shaped public inputs are validated at the API boundary. `properties_json`
and `traits_json` must be valid JSON objects, and `value_json` must be a valid
JSON value. Malformed JSON returns `HONCH_ERROR_INVALID_ARGUMENT`; C/POSIX
intentionally fails closed instead of silently dropping invalid user properties.

When `battery_callback` is configured, valid readings are stamped as
`$battery_level`. The SDK queues `$battery_low` once when the level drops below
`battery_low_threshold`, then arms it again after the level recovers.

`auto_properties_callback` lets platform adapters add automatic event
properties without putting platform-specific code in the reusable core. The
callback receives a typed sink and can add raw JSON values:

```c
static honch_status_t add_platform_properties(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    int rssi = *(int *)userdata;
    char value[16];
    snprintf(value, sizeof(value), "%d", rssi);
    return sink(sink_ctx, "$wifi_rssi", value);
}
```

The SDK validates each key and JSON value before appending it. Adapter
properties are added after user properties and before SDK-owned properties, so
core-owned values such as `$device_id`, `$sdk_platform`, and `$firmware_version`
cannot be overridden by either user input or an adapter. The callback runs while
the SDK is constructing an event, so it should return quickly and must not call
back into Honch APIs for the same client.

Background flushing is enabled by default to match the ESP-IDF SDK. When
`flush_interval_seconds` or `flush_event_threshold` are zero, the SDK uses the
ESP-IDF defaults of 60 seconds and 30 queued events. Set
`disable_background_flush` nonzero to keep events queued until an explicit
`honch_flush` call. Retryable transport failures use exponential backoff with
jitter. Shutdown always attempts a synchronous best-effort flush for a valid
client, even when background flushing is disabled.

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
        .flush_event_threshold = 30,
        .disable_background_flush = 0,
        .battery_callback = NULL,
        .battery_low_threshold = 15
    };

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        return 1;
    }

    honch_identify(client, "user-123", "{\"plan\":\"beta\"}");
    honch_session_start(client, "recording");
    honch_track(client, "recording_started", "{\"mode\":\"hdr\",\"resolution\":\"4k\"}");
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

- events are written atomically
- startup removes temporary write files
- queue length is bounded by `max_queued_events`
- flush requests are sent in batches capped at 50 events
- when full, the oldest event is dropped before accepting a new event
- retryable failures keep files in `pending/`
- permanent rejections move attempted files to `dead/`
- `honch_reset` rotates identity state and clears pending/dead queues as a
  factory-reset boundary

## Test Coverage

Current C tests cover:

- init validation
- event persistence
- ISO-8601 timestamp encoding
- tokenized batch envelope encoding
- gzip-compressed transport requests
- strict JSON validation for public property input
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
- multi-batch flush
- permanent rejection dead-letter behavior
- reset queue clearing
- reset identity behavior
- opt-in real capture E2E flush

Run:

```sh
ctest --test-dir build --output-on-failure
```
