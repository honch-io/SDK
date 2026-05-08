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
- opt-in background flushing with retry backoff and jitter
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

The real ingest E2E test is opt-in and requires a capture endpoint and token.
It is not run by default because it sends events to the configured service.

```sh
export HONCH_E2E_ENDPOINT="https://capture.example.com"
export HONCH_E2E_TOKEN="honch_..."
cmake -S . -B build-e2e -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_E2E=ON
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure -R honch_c_core_e2e
```

To verify the full local pipeline through ClickHouse, also provide the project
ID and ClickHouse endpoint:

```sh
export HONCH_E2E_ENDPOINT="http://127.0.0.1:8001"
export HONCH_E2E_TOKEN="honch_..."
export HONCH_E2E_PROJECT_ID="<project-id>"
export HONCH_E2E_CLICKHOUSE_URL="http://127.0.0.1:8123"
export HONCH_E2E_CLICKHOUSE_DATABASE="platform"
ctest --test-dir build-e2e --output-on-failure -R honch_c_core_e2e
```

When the ClickHouse variables are set, the test sends a unique event and polls
ClickHouse until that event appears. This is the mode CI should use when it
starts the local capture, Pub/Sub emulator, worker, and ClickHouse stack.

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
void honch_shutdown(honch_client_t *client);
const char *honch_get_device_id(honch_client_t *client);
honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
const char *honch_status_string(honch_status_t status);
```

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
- `flush_interval_seconds`: enables interval-based background flushing when nonzero
- `flush_event_threshold`: enables threshold-based background flushing when nonzero
- `flush_retry_initial_ms`: defaults to `1000`
- `flush_retry_max_ms`: defaults to `300000`
- `battery_callback`: returns `0`-`100`, or negative when unknown
- `battery_low_threshold`: defaults to `15`

`honch_get_device_id` returns a borrowed pointer for single-threaded callers.
The returned pointer is owned by the SDK and remains valid until `honch_reset`
or `honch_shutdown`. Use `honch_copy_device_id` when another thread may reset or
shut down the client while the ID is being read.

`honch_session_start` starts an in-memory analytics session, queues a
`$session_start` event, and attaches the generated `$session_id` to later
events until `honch_session_end` queues `$session_end`.

Auto-stamped property keys such as `$device_id`, `$session_id`, and
`$sdk_platform` are owned by the SDK. Per-event properties using those keys are
ignored so the SDK-stamped values win.

`honch_set_property` queues a `$set_property` event whose properties contain the
provided key/value pair. It does not persist context onto future events.

When `battery_callback` is configured, valid readings are stamped as
`$battery_level`. The SDK queues `$battery_low` once when the level drops below
`battery_low_threshold`, then arms it again after the level recovers.

Background flushing is opt-in for C/POSIX. Set `flush_interval_seconds` or
`flush_event_threshold` to start a worker thread. Retryable transport failures
use exponential backoff with jitter, and shutdown performs a synchronous flush
when the background worker is enabled.

GPIO tracking is intentionally kept out of the reusable C core. Use a
platform adapter, like `example/posix_gpio`, to translate GPIO edge samples into
normal `honch_track` calls.

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
