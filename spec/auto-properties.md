# Auto-Stamped Properties

Every event expanded by Capture carries this context. SDKs provide required
context through wire-v2 batch context and dynamic per-event properties.
User-supplied properties using SDK-owned keys are rejected instead of
overwritten.

Honch SDKs are not silent on the wire. These context fields and lifecycle rules
mean auto-emitted events create analytics traffic and queue pressure even before
the host application records its own product events.

## Required (always present)

| Property | Type | Description |
|----------|------|-------------|
| `$device_id` | string | Stable device identifier (platform-specific derivation) |
| `$device_model` | string | From SDK config |
| `$firmware_version` | string | From SDK config |
| `$sdk_platform` | string | SDK platform identifier (e.g. `"esp-idf"`, `"ios"`, `"android"`) |
| `$sdk_version` | string | SDK behavior version (e.g. `"0.2.0"`) |
| `$environment` | string | From SDK config, defaults to `"production"` |

## Conditional

| Property | Type | Condition |
|----------|------|-----------|
| `$session_id` | string | Only when a session is active |
| `$battery_level` | int (0-100) | Only when `battery_callback` is configured and returns >= 0 |
| `$wifi_rssi` | int (dBm) | Only when a port auto-properties callback supplies it; the portable core does not auto-detect Wi-Fi |

Ports may supply additional non-reserved auto properties through
`auto_properties_callback`; reserved SDK-owned keys are ignored except
`$wifi_rssi`.

## Server-Side Promotion

Capture promotes these fields to top-level columns: `$device_id`,
`$device_model`, `$firmware_version`, `$session_id`, `$sdk_platform`,
`$environment`, `$sdk_version`. Promoted context properties are encoded once in
wire-v2 message context and removed from the event properties before wire
encoding. Port authors should not also write promoted context fields into event
properties.

## Lifecycle Events

These events are emitted automatically by the SDK:

| Event | When | Extra Properties |
|-------|------|-----------------|
| `$device_boot` | End of `init()` | `reset_reason` (string) |
| `$device_shutdown` | Start of `shutdown()` | — |
| `$firmware_update` | Boot, if version changed | `previous_version`, `new_version` |
| `$battery_low` | Battery drops below threshold | `level` (int) |
| `$session_start` | `session_start()` called | `session_name` (string, optional) |
| `$session_end` | `session_end()` called | — |

`reset()` clears SDK identity, state, and queued events. It does not enqueue a
`$device_reset` lifecycle event.

Ports may expose explicit connectivity helpers that enqueue
`$connectivity_change` with `state`: `"connected"` or `"disconnected"`.
Connectivity changes are not auto-detected by the portable core.

### Reset Reason Values (embedded SDKs)

| Value | Description |
|-------|-------------|
| `"power_on"` | Normal power-on |
| `"software"` | Software reset |
| `"panic"` | Panic/crash |
| `"watchdog"` | Watchdog timer |
| `"deepsleep"` | Wake from deep sleep |
| `"brownout"` | Brownout detector |
| `"unknown"` | Other/unrecognized |

## Explicitly Excluded

These are **not** auto-stamped (noise on every event):
- `$free_heap_bytes`
- `$uptime_seconds`
- `$hardware_revision`
