# Honch MicroPython SDK

Stable MicroPython wrapper for the canonical Honch C core. The Python package keeps a small `honch.Honch` API while behavior comes from the same shared sources used by the ESP-IDF and POSIX ports.

## Status

Stable `0.2.0`. The SDK is not standalone pure Python; firmware must be built with the `_honch_core` user C module.

This port targets MicroPython. CircuitPython is not covered by the current user C module build flow.

## Build Into MicroPython

From a MicroPython checkout, build a port with the Honch user module:

```bash
make -C ports/unix \
  USER_C_MODULES=/path/to/SDK/ports/micropython/usermod/honch/micropython.cmake
```

For board firmware, pass the same user module path and freeze the wrapper:

```bash
make BOARD=MYBOARD \
  USER_C_MODULES=/path/to/SDK/ports/micropython/usermod/honch/micropython.cmake \
  FROZEN_MANIFEST=/path/to/SDK/ports/micropython/manifest.py
```

`mip` can install wrapper files from package metadata, but those files require firmware that already contains `_honch_core`.
Do not install the `honch/` wrapper into `/lib` when it is already frozen into
the firmware. Keeping both copies wastes the board filesystem and can make
MicroPython import an older `/lib/honch` package instead of the frozen SDK.

## Basic Usage

```python
import honch

client = honch.Honch(
    api_key="project-key",
    endpoint_url="https://capture.honch.io",
    device_model="ActionCam X1",
    firmware_version="1.2.3",
    event_buffer=bytearray(8192),
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
- `event_buffer`

Optional:

- `device_id`
- `environment`
- `batch_size`
- `max_queued_events`
- `max_event_bytes`
- `transport_timeout_ms`
- `flush_interval_seconds`
- `flush_min_interval_ms` (default `10000`; use `0xFFFFFFFF` to disable for benchmarks)
- `flush_event_threshold`
- `flush_retry_initial_ms`
- `flush_retry_max_ms`
- `battery_low_threshold`
- `connectivity_callback` (return false while offline; ticks are skipped and `flush()` raises `OfflineError`)

Python `platform=`, `transport=`, `battery_callback=`, and `auto_properties_callback=` hooks are not supported by the C-core-derived port. Board behavior belongs in the user module adapters.

client.tick() and client.flush() may block for up to the configured transport
timeout because urequests.post holds the MicroPython interpreter while the HTTP
request is in progress. Do not call tick() from a latency-sensitive control
loop, timing-critical sensor loop, UI refresh path, or watchdog-sensitive
section. Schedule it from a low-priority part of the program where a stalled
interpreter is acceptable for the configured timeout.
Each scheduled tick posts at most one wire chunk, so large queued uploads may
need several pump iterations to finish.

Do not call `tick()` while WLAN is disconnected or the radio is intentionally
off. If your loop cannot guarantee that, pass `connectivity_callback`; it should
be fast and read host-owned connectivity state.

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
client.tick()
client.flush()
client.reset()
client.shutdown()
client.get_device_id()
client.queue_stats()
```

Call `client.tick()` periodically from the device main loop for scheduled flush work. Use `client.flush()` when you want an immediate drain attempt.

Properties support typed event values, including strings, integers, floats,
booleans, lists, dictionaries, null, and `bytes`. Capture may reject bytes
unless the project enables binary properties. SDK-owned auto property keys
supplied by users are rejected before compact wire-v2 packetization.

## Storage And Transport

The default user C module stores queued events in the caller-provided
`event_buffer`. Events, `identify()` state, and firmware-version state are
volatile by default and are lost across reset or power loss. Device id defaults
to `machine.unique_id()` when `device_id` is not supplied.

Flush sends compact chunk frames to `POST <endpoint_url>/capture` with `Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and `X-Honch-Stream-Id`.

## Security

Use HTTPS in production. Verify the board has network, time, and trust setup needed for certificate validation. Keep project keys out of source control, logs, and event properties.

## Tests

```bash
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=ports/micropython python3 -m unittest discover \
  -s ports/micropython/tests -t . -v
```

Full runtime validation requires building MicroPython with `_honch_core` and running the wrapper on that interpreter or firmware.
