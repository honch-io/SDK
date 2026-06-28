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
| `$sdk_version` | string | SDK behavior version (e.g. `"0.2.3"`) |
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
| `$crash` | End of `init()`, when the SDK recovers a crash from the previous boot | `source` (crash kind), `severity`, `reset_reason`, optional `message`, optional `component`, optional `task_name`, optional `exception_cause`, optional `fault_pc`, optional `fault_addr`, optional `backtrace`, optional `firmware_build_id`, optional `summary_version`, optional `coredump_available` |
| `$error` | When the firmware reports an error-level log/condition (automatic, no manual call) | `level`, optional `component`, `message`, `count` |
| `$device_shutdown` | Start of `shutdown()` | — |
| `$firmware_update` | Boot, if version changed | `previous_version`, `new_version` |
| `$battery_low` | Battery drops below threshold | `level` (int) |
| `$session_start` | `session_start()` called | `session_name` (string, optional) |
| `$session_end` | `session_end()` called | — |

Error and crash reporting is **automatic**: the host application makes no manual
call. There is no public `report_error` API. Capture is on by default and is
removed only at build time (see the build-strip note below).

### `$crash`

`$crash` is the SDK's record of the firmware having died — a panic, watchdog
reset, brownout, stack overflow, failed assert, CPU lockup, unhandled exception
(MicroPython), or fatal signal (POSIX). It is emitted **once** during the next
`init()` after the crash, built by the port from whatever its platform's native
fault machinery can provide (ESP-IDF coredump summary, an MCU fault record, a
POSIX signal breadcrumb). `source` carries the crash kind (`panic`, `watchdog`,
`assert`, `brownout`, `stack_overflow`, `hardfault`, `lockup`, `exception`,
`signal`, `unknown`). Only `source`/`reset_reason` are guaranteed; richer fields
appear when the platform can supply them, so fidelity is tiered by platform
capability.

Because the crash is only detected on the recovery boot, the `$crash` event
timestamp is the recovery-boot time, not the original crash time.

`$crash` is **once-only** per client lifetime and **erase-after-ack**: the port
clears the on-device crash source (e.g. erases the ESP-IDF coredump partition,
unlinks the POSIX breadcrumb) only after the `$crash` event has been delivered to
Capture, so a crash is neither lost nor re-reported on every subsequent boot.

`$crash` is not a coredump, register dump, full stack capture, minidump,
symbolication pipeline, or crash forensics system. Code-sensitive symbolication
context is separately opt-in where a port exposes it: ports may send
`exception_cause`, `firmware_build_id`, `fault_pc`, `fault_addr`, `backtrace`,
`task_name`, `summary_version`, and `coredump_available` when crash
symbolication context is enabled and platform crash-summary metadata is
available. These are raw identifiers, reasons, and addresses for **server-side**
symbolication; the SDK does not send source code, source paths, symbol files,
RAM snapshots, or full coredumps. (`coredump_available` is a forward-compatible
signal that a raw image remains on the device for a future, separately specified
upload path.)

`$crash` emission is best-effort: if the SDK cannot build or enqueue it during
`init()` because local queue/storage limits are tight, the SDK still completes
init and continues normal telemetry. Diagnostics failure must never become a
device startup failure.

### `$error`

`$error` is automatic logged-error capture. A port hooks its platform's
error-level logging path (e.g. `ESP_LOGE` via `esp_log_set_vprintf`) so an error
the firmware logs becomes a bounded `$error` event with `level="error"`, the log
`component`/tag, and a truncated `message` — with no application code change.
Errors are **bounded and coalesced**: a small fixed dedup table (default 4 slots)
collapses identical `(component, message)` errors into one event carrying a
`count`, and is drained to the queue on each flush/tick/shutdown. Distinct errors
that overflow the table while it is full are counted and surfaced as a `dropped`
property on the next emitted `$error`, so loss is observable rather than silent.
The SDK's own internal logs are never re-reported as `$error` (recursion guard).
`$error` events ride the normal queue and flush policy; they never force an
immediate upload and are never emitted from an ISR.

### Build-strip modularity

Error/crash reporting is build-strip modular. `HONCH_ENABLE_ERROR_TRACKING` is
the umbrella; `HONCH_ENABLE_CRASH_CAPTURE` and `HONCH_ENABLE_LOG_CAPTURE` are
sub-toggles that default to it. When a toggle is `0` (or an equivalent port build
option is used), the corresponding emission path is compiled out entirely with no
flash/RAM cost. There is no runtime opt-in flag.

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
| `"interrupt_wdt"` | Interrupt watchdog |
| `"task_wdt"` | Task watchdog |
| `"deep_sleep"` | Wake from deep sleep |
| `"brownout"` | Brownout detector |
| `"external"` | External reset |
| `"sdio"` | SDIO reset |
| `"usb"` | Native USB reset |
| `"jtag"` | JTAG reset |
| `"efuse"` | eFuse error reset |
| `"power_glitch"` | Power glitch detector |
| `"unknown"` | Other/unrecognized |

## Explicitly Excluded

These are **not** auto-stamped (noise on every event):
- `$free_heap_bytes`
- `$uptime_seconds`
- `$hardware_revision`
