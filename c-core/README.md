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
- opt-in background flushing with retry backoff and jitter
- persisted firmware version change detection
- persistent event context properties
- battery level auto-stamping and low-battery lifecycle events
- automatic core lifecycle events for boot, shutdown, and reset
- explicit event tracking and flushing
- reset behavior for factory-reset-style identity rotation
- local mock collector for development
- deterministic C tests using fake transport hooks
- minimal POSIX example
- connected camera usage example

Temporary development behavior:

- the local mock collector path still uses JSON
- the current transport implementation is a development harness, not the final CBOR ingest contract

Do not optimize around the current JSON ingest API as the long-term contract. The package is expected to move to CBOR when the shared ingest API/spec update lands.

## Layout

```text
include/honch/          Public C headers
src/                    SDK implementation and internal modules
tests/                  C test executable
examples/posix_device/  Minimal smoke example
examples/connected_camera/
tools/mock_collector.py Local development collector
AGENTS.md              Package-specific agent/workflow rules
```

## Build

Requirements:

- CMake 3.20+
- C11 compiler
- libcurl
- zlib
- Python 3 for the mock collector

From this directory:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The C targets build with warnings enabled and warnings treated as errors.

## Local Smoke Test

Start the local collector:

```sh
python3 tools/mock_collector.py --port 8765
```

Run the minimal example:

```sh
./build/examples/posix_device/honch_posix_example
```

Run the connected camera example:

```sh
./build/examples/connected_camera/honch_connected_camera_example
```

The mock collector prints only summaries, for example:

```json
{"accepted": 5}
```

It does not print API keys, traits, or event payloads.
It accepts the SDK's gzip-compressed JSON requests.

## Public API

Header:

```text
include/honch/honch.h
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
const char *honch_status_string(honch_status_t status);
```

Required config:

- `api_key`
- `endpoint_url`
- `queue_directory`

Optional config:

- `device_id`: generated and persisted when omitted
- `device_model`
- `firmware_version`
- `environment`: defaults to `production`
- `batch_size`
- `max_queued_events`
- `max_event_bytes`
- `transport_timeout_ms`
- `flush_interval_seconds`: enables interval-based background flushing when nonzero
- `flush_event_threshold`: enables threshold-based background flushing when nonzero
- `flush_retry_initial_ms`: defaults to `1000`
- `flush_retry_max_ms`: defaults to `300000`
- `battery_callback`: returns `0`-`100`, or negative when unknown
- `battery_low_threshold`: defaults to `15`

`honch_get_device_id` returns the active device ID for the client, or `NULL` for
an invalid client. The returned pointer is owned by the SDK and remains valid
until `honch_reset` or `honch_shutdown`.

`honch_session_start` starts an in-memory analytics session, queues a
`$session_start` event, and attaches the generated `$session_id` to later
events until `honch_session_end` queues `$session_end`.

When `battery_callback` is configured, valid readings are stamped as
`$battery_level`. The SDK queues `$battery_low` once when the level drops below
`battery_low_threshold`, then arms it again after the level recovers.

Background flushing is opt-in for C/POSIX. Set `flush_interval_seconds` or
`flush_event_threshold` to start a worker thread. Retryable transport failures
use exponential backoff with jitter, and shutdown performs a synchronous flush
when the background worker is enabled.

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
    properties/            persistent event context values
```

Queue behavior:

- events are written atomically
- startup removes temporary write files
- queue length is bounded by `max_queued_events`
- when full, the oldest event is dropped before accepting a new event
- retryable failures keep files in `pending/`
- permanent rejections move attempted files to `dead/`
- `honch_reset` clears pending/dead queues before queuing a fresh `$device_reset`
  event and rotating identity state

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
- persistent properties on future events
- session start/end events and `$session_id` event context
- boot, shutdown, and reset lifecycle events
- firmware update detection
- battery callback, `$battery_level`, and `$battery_low`
- identify payload and persisted `distinct_id`
- bounded queue drop-oldest behavior
- retryable flush preserving pending events
- background threshold flush and retry backoff
- multi-batch flush
- permanent rejection dead-letter behavior
- reset queue clearing while preserving `$device_reset`
- reset identity behavior

Run:

```sh
ctest --test-dir build --output-on-failure
```
