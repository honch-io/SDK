# Honch MicroPython SDK

MicroPython SDK for Honch analytics on connected hardware.

This package implements the shared Honch SDK contract from `../spec/`:

- gzip-compressed JSON `POST <endpoint>/batch`
- persistent local queue before delivery
- persistent device and distinct identity
- automatic lifecycle events and auto-stamped properties
- bounded batching, retry preservation, and dead-letter handling

## Status

Initial v1 implementation. The public API is intentionally small and centered on
an explicit `Honch` client instance.

## Install

For development on a connected board:

```sh
mpremote mount . run examples/basic.py
```

For firmware builds, include the package through `manifest.py`:

```sh
make BOARD=MYBOARD FROZEN_MANIFEST=/path/to/micropython/manifest.py
```

For `mip`/`mpremote mip` workflows, use `package.json` from this directory when
publishing or installing from a hosted repository.

## Basic Usage

```python
import honch

client = honch.Honch(
    api_key="your-api-key",
    endpoint_url="https://capture.honch.io",
    device_model="ActionCam X1",
    firmware_version="1.2.3",
    queue_directory="/honch",
)

client.identify("user-123", {"plan": "beta"})
client.session_start("recording")
client.track("recording_started", {"mode": "hdr"})
client.session_end()
client.flush()
client.shutdown()
```

## Configuration

Required:

- `api_key`
- `endpoint_url`
- `device_model`
- `firmware_version`
- `queue_directory`

Optional:

- `device_id`: configured identity; generated and persisted when omitted
- `environment`: defaults to `"production"`
- `batch_size`: defaults to `20`, capped at `50`
- `max_queued_events`: defaults to `1000`
- `max_event_bytes`: defaults to `16384`
- `transport_timeout_ms`: defaults to `10000`
- `flush_interval_seconds`: defaults to `60`
- `flush_event_threshold`: defaults to `30`
- `flush_retry_initial_ms`: defaults to `1000`
- `flush_retry_max_ms`: defaults to `300000`
- `disable_background_flush`: defaults to `False`
- `battery_callback`: returns `0`-`100`, or negative/`None` when unknown
- `battery_low_threshold`: defaults to `15`
- `auto_properties_callback`: returns a dict of JSON-compatible adapter properties
- `platform`: optional board adapter
- `transport`: optional HTTP adapter

## Public API

```python
client.track(event_name, properties=None)
client.identify(distinct_id, traits=None)
client.set_property(key, value=None)
client.session_start(session_name=None)
client.session_end()
client.connectivity_changed(connected)
client.connected()
client.disconnected()
client.flush()
client.reset()
client.shutdown()
client.get_device_id()
```

`properties` and `traits` must be dictionaries containing JSON-compatible
values. SDK-owned auto properties win over user-supplied properties with the
same key.

`auto_properties_callback`, when configured, must return `None` or a dictionary
with string keys and JSON-compatible values. SDK-owned auto properties still win
over callback-supplied values, except `$wifi_rssi`, which platform adapters may
provide.

### Core Contract Mapping

The shared SDK sheet describes six functions. MicroPython exposes the same
contract through an explicit client object:

| Shared contract | MicroPython API |
| --- | --- |
| `init(config)` | `client = honch.Honch(**config)` |
| `track(event_name, properties)` | `client.track(event_name, properties)` |
| `identify(distinct_id, properties)` | `client.identify(distinct_id, traits)` |
| `set_property(key, value)` | `client.set_property(key, value)` |
| `flush()` | `client.flush()` |
| `reset()` | `client.reset()` |

`session_start()`, `session_end()`, `connectivity_changed()`, `shutdown()`, and
`get_device_id()` are MicroPython lifecycle and inspection extensions around
that core contract.

## Storage Layout

The SDK stores state below `queue_directory`:

```text
pending/
dead/
state/
  device_id
  distinct_id
  firmware_version
```

Events are written before delivery. Retryable network, `429`, and `5xx` errors
leave pending events intact. Permanent `4xx` rejection moves attempted events to
`dead/`.

## Gzip

The shared wire format requires gzip. If the active MicroPython build does not
provide gzip compression, events remain queued and `flush()` raises
`CompressionUnavailableError`. Inject a platform adapter with `gzip_compress()`
for boards that provide compression through another module.

## Background Flush

Threshold flushing is notification-driven. When `flush_event_threshold` is
reached, the SDK calls an optional `platform.request_flush(callback)` adapter;
the adapter should schedule `callback` to run later on the board's timer, event
loop, or worker primitive. It must not call the callback inline from `track()`.

Interval flushing uses an optional
`platform.start_periodic(interval_ms, callback)` adapter. Explicit `flush()` is
always supported and is the fallback when no background adapter is available.

## Connectivity Changes

Board network code can notify the SDK when connectivity changes:

```python
client.connected()
client.disconnected()
client.connectivity_changed(True)
client.connectivity_changed(False)
```

Each state transition queues a `$connectivity_change` lifecycle event with
`state` set to `"connected"` or `"disconnected"`. Duplicate notifications for
the current state are ignored. A transition to connected asks the scheduler for
a deferred flush through `platform.request_flush(callback)` when background
flush is enabled; the SDK does not perform network work inline from the
notification call.

## Relay Mode

This package does not currently implement a relay drain API or sealed relay
envelope. The ESP-IDF and C/POSIX SDKs do not expose that transport contract
yet, and the shared relay byte format is still reserved for future work.

Honch remains a payload, not a BLE, GATT, USB, UART, pairing, or customer packet
protocol. Until the shared relay contract is implemented across SDKs,
MicroPython supports direct HTTPS flush only.

## Footprint Notes

The source package is about 27 KB across the current `honch/*.py` modules before
`.mpy` compilation, filesystem metadata, or firmware freezing. Exact flash and
RAM use depends on the board firmware, MicroPython port, enabled modules, and
whether the package is frozen into the image.

Queue operations keep memory bounded by reading at most `batch_size` events per
flush, capped at 50, and by rejecting events larger than `max_event_bytes`.
Pending and dead-letter events are persisted as individual JSON files below
`queue_directory`, so storage cost scales with queued event count and event
size.

Direct flush requires gzip support. If the active port lacks gzip compression,
events remain queued and `flush()` raises `CompressionUnavailableError`; provide
a platform adapter with `gzip_compress()` when the board exposes compression
through another module or native binding.

## Tests

Host-runnable checks use the Python standard library:

```sh
python -m unittest discover -v
```

The tests use fake transports, clocks, randomness, and temporary queue
directories. They do not perform real network calls.

Real capture E2E is opt-in and uses the same local-stack defaults as the
C/POSIX SDK:

```sh
HONCH_E2E=1 python -m unittest tests.test_e2e_capture -v
```

Defaults:

- `HONCH_E2E_ENDPOINT`: `http://127.0.0.1:8001`
- `HONCH_E2E_CAPTURE_HEALTH_URL`: `http://127.0.0.1:8001/health`
- `HONCH_E2E_WORKER_HEALTH_URL`: `http://127.0.0.1:8080/`
- `HONCH_E2E_TOKEN`: `honch_e2e_test_key`
- `HONCH_E2E_PROJECT_ID`: `00000000-0000-0000-0000-000000000002`
- `HONCH_E2E_CLICKHOUSE_URL`: `http://127.0.0.1:8123`
- `HONCH_E2E_CLICKHOUSE_DATABASE`: `platform`
