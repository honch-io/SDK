# Honch MicroPython SDK

MicroPython wrapper for the canonical Honch C core. The Python package keeps the
small `honch.Honch` API, while SDK behavior comes from the same `core/` sources
used by the ESP-IDF and POSIX ports: event semantics, CBOR, identity,
lifecycle, compact wire encoding, queue policy, retry classification, and
packetization.

## Status

The MicroPython port is now C-core-derived. It is not a standalone pure-Python
SDK and must be built into firmware with the `_honch_core` user C module.

## Build Into MicroPython

From a MicroPython checkout, build a port with this repository's user module:

```sh
make -C ports/unix USER_C_MODULES=/path/to/SDK/ports/micropython/usermod/honch/micropython.cmake
```

For board firmware, pass the same `USER_C_MODULES` path to the target port
build. Freeze the thin Python wrapper with `manifest.py`:

```sh
make BOARD=MYBOARD \
  USER_C_MODULES=/path/to/SDK/ports/micropython/usermod/honch/micropython.cmake \
  FROZEN_MANIFEST=/path/to/SDK/ports/micropython/manifest.py
```

`mip` can install the wrapper files from `package.json`, but those files require
firmware that already contains `_honch_core`.

The module reserves a 64 KiB C heap by default for ports such as `rp2`, where
runtime `malloc`/`free` is unavailable unless firmware sets
`MICROPY_C_HEAP_SIZE`. The shared Honch C core uses C allocation for client
state, event buffers, CBOR packetization, and queue processing.

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

- `device_id`
- `environment`
- `batch_size`
- `max_queued_events`
- `max_event_bytes`
- `transport_timeout_ms`
- `flush_interval_seconds`
- `flush_event_threshold`
- `flush_retry_initial_ms`
- `flush_retry_max_ms`
- `disable_background_flush`
- `battery_low_threshold`

Python `platform=`, `transport=`, `battery_callback=`, and
`auto_properties_callback=` hooks are not supported by the C-core-derived port.
Board behavior belongs in the C user-module adapters.

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
values. SDK-owned auto properties are stamped by the C core and win over
user-supplied properties with the same key.

## Storage And Transport

The C user module owns the MicroPython storage and transport adapters. It stores
state and queue files below `queue_directory` using the same logical layout as
the other C-core ports:

```text
pending/
dead/
state/
  device_id
  distinct_id
  firmware_version
```

Flush sends compact chunk wire frames to `POST <endpoint_url>/capture` with
`Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and
`X-Honch-Stream-Id`. Retry and dead-letter behavior is inherited from the
canonical C core. Capture also accepts the same format on `/e` and `/chunks`.

## Tests

Host-side tests verify the wrapper and user-module source shape:

```sh
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=ports/micropython python3 -m unittest discover \
  -s ports/micropython/tests -t . -v
```

Full runtime validation requires building MicroPython with
`ports/micropython/usermod/honch/micropython.cmake` and running the wrapper on
that interpreter or firmware.

After building the MicroPython unix port with `_honch_core`, run the runtime
smoke test with:

```sh
MICROPYTHON_BIN=/path/to/micropython/ports/unix/build-standard/micropython \
  ports/micropython/scripts/run-unix-tests.sh
```

The runner first imports `_honch_core`, then executes a MicroPython-compatible
smoke test against the real `honch.Honch` wrapper.
