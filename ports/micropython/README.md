# Honch MicroPython SDK

Stable MicroPython wrapper for the canonical Honch C core. The Python package keeps a small `honch.Honch` API while behavior comes from the same shared sources used by the ESP-IDF and POSIX ports.

## Status

Stable `0.3.0`. The SDK is not standalone pure Python; firmware must be built with the `_honch_core` user C module.

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

The Honch user C module does not set firmware-global MicroPython options such
as `MICROPY_C_HEAP_SIZE`. Keep heap sizing in the board or host firmware
configuration. If your board needs additional C heap for Honch and other native
modules, opt into that value in the board config rather than in the user module.

`mip` can install wrapper files from package metadata, but those files require firmware that already contains `_honch_core`.
Do not install the `honch/` wrapper into `/lib` when it is already frozen into
the firmware. Keeping both copies wastes the board filesystem and can make
MicroPython import an older `/lib/honch` package instead of the frozen SDK.

## Basic Usage

```python
import honch

client = honch.Honch(
    api_key="project-key",
    endpoint_url="https://i.honch.io",
    device_id="device-serial-001",
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
- `device_id`
- `device_model`
- `firmware_version`
- `event_buffer`

Optional:
- `endpoint_url` (default `https://i.honch.io`)
- `environment`
- `batch_size`
- `max_queued_events`
- `max_event_bytes`
- `transport_timeout_ms` (finite positive milliseconds, clamped to 10000)
- `flush_interval_seconds`
- `flush_min_interval_ms` (default `15000`; use `0xFFFFFFFF` to disable for benchmarks)
- `flush_event_threshold`
- `flush_retry_initial_ms`
- `flush_retry_max_ms`
- `battery_low_threshold`
- `connectivity_callback` (return false while offline; ticks are skipped and `flush()` raises `OfflineError`)

Python `platform=`, `transport=`, `battery_callback=`, and `auto_properties_callback=` hooks are not supported by the C-core-derived port. Board behavior belongs in the user module adapters.

client init does synchronous work on the caller's thread. It validates config,
initializes the C core against the caller-provided event buffer, derives initial
state from the supplied device/config values, and queues `$device_boot` before
returning. It does not perform network I/O; delivery remains cooperative through
`tick()` or explicit `flush()` calls.

Crash reporting is automatic. `client.install_error_hook()` installs a
best-effort `sys.excepthook` wrapper (on runtimes that expose that hook) that
emits a one-time `$crash` event for an uncaught exception (`source="exception"`,
with the exception type in `exception_cause`); the original exception is always
delivered to the previous hook. `client.report_log_error(message,
component=...)` emits a bounded, coalesced `$error` event and is intended to be
driven automatically (e.g. from a `logging.Handler`), not sprinkled through
application code. The port does not install panic hooks or board-specific
reset-reason adapters; board reset telemetry belongs in the user module adapter.

client.tick() and client.flush() may block for up to the configured transport
timeout because urequests.post holds the MicroPython interpreter while the HTTP
request is in progress. Do not call tick() from a latency-sensitive control
loop, timing-critical sensor loop, UI refresh path, or watchdog-sensitive
section. Do not call tick() or flush() from ISR-adjacent callbacks or
high-priority tasks. Schedule it from a low-priority part of the program where a
stalled interpreter is acceptable for the configured timeout. Explicit timeout
values above the hard maximum of 10000 ms are clamped.
Each scheduled tick posts at most one wire chunk, so large queued uploads may
need several pump iterations to finish.

Do not call `tick()` while WLAN is disconnected or the radio is intentionally
off. If your loop cannot guarantee that, pass `connectivity_callback`; it should
be fast and read host-owned connectivity state.

## Public API

```python
client.track(event_name, properties=None)
client.report_log_error(message, component=None)
client.install_error_hook()
client.uninstall_error_hook()
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

After `identify()`, future events use the new distinct ID. The `$identify`
event also includes the previous identity as `$anon_distinct_id`, usually the
device ID, so earlier anonymous events can merge into the identified person.

Call `client.tick()` periodically from the device main loop for scheduled flush work. Use `client.flush()` when you want an immediate drain attempt.

Properties support typed event values, including strings, integers, floats,
booleans, lists, dictionaries, null, and `bytes`. Capture may reject bytes
unless the project enables binary properties. SDK-owned auto property keys
supplied by users are rejected before compact wire-v2 packetization.

## Storage And Transport

The default user C module stores queued events in the caller-provided
`event_buffer`. Events, `identify()` state, and firmware-version state are
volatile by default and are lost across reset or power loss. `device_id` is required
because this port does not persist SDK identity. Pass a caller-provided stable
device_id from board provisioning, NVS, a filesystem file, or another
product-owned durable source.

That RAM queue is bounded and compact. Consuming a non-tail event uses an O(n)
memmove per consumed event to keep the caller-provided buffer contiguous; this
is acceptable at default sizes, but larger buffers and queue limits should be
measured on the target board.

Flush sends compact chunk frames to `POST <endpoint_url>/capture` with `Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and `X-Honch-Stream-Id`.

## Security

Use HTTPS in production. Verify the board has network, time, and trust setup needed for certificate validation. Keep project keys out of source control, logs, and event properties.

## Tests

```bash
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=ports/micropython python3 -m unittest discover \
  -s ports/micropython/tests -t . -v
```

Full runtime validation requires building MicroPython with `_honch_core` and running the wrapper on that interpreter or firmware.
