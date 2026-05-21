# Auto-Stamped Properties

Every event emitted by a Honch SDK must include these properties. They are set automatically by the SDK encoder — user-supplied properties with the same key are overwritten.

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
| `$wifi_rssi` | int (dBm) | Only when Wi-Fi is connected (embedded SDKs only) |

## Server-Side Promotion

Capture promotes these properties to top-level columns: `$device_id`, `$device_model`, `$firmware_version`, `$session_id`, `$sdk_platform`, `$environment`. The SDK does not need to do anything special — just put them in `properties`.

## Lifecycle Events

These events are emitted automatically by the SDK:

| Event | When | Extra Properties |
|-------|------|-----------------|
| `$device_boot` | End of `init()` | `reset_reason` (string) |
| `$device_shutdown` | Start of `shutdown()` | — |
| `$connectivity_change` | Wi-Fi connect/disconnect | `state`: `"connected"` or `"disconnected"` |
| `$firmware_update` | Boot, if version changed | `previous_version`, `new_version` |
| `$battery_low` | Battery drops below threshold | `level` (int) |
| `$session_start` | `session_start()` called | `session_name` (string, optional) |
| `$session_end` | `session_end()` called | — |
| `$device_reset` | `reset()` called | — |

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
