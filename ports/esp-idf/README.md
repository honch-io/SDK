# Honch ESP-IDF SDK

Stable ESP-IDF component for Honch product analytics on connected hardware.

Events are queued locally and sent to Capture as compact chunk wire frames.
The default ESP-IDF integration is RAM-only by default for low `honch_track()`
latency. Events are not preserved across reset or power loss unless the
integrator supplies durable queue storage through `event_queue_ops`.

## Status

Stable `0.2.1`.

## Requirements

- ESP-IDF >= 5.0
- An ESP32 dev board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- A Honch project key.
- Host-owned networking initialized before expecting HTTPS delivery:
  `esp_netif_init()`, the default event loop if your Wi-Fi stack uses it,
  Wi-Fi, time, and TLS trust.

## Add to your project

**Option A: ESP Component Manager**

```
idf.py add-dependency "honch-io/honch^0.2.1"
```

**Option B: Git submodule**

```bash
git submodule add https://github.com/honch-io/SDK.git components/honch
```

The ESP-IDF component package uses the repository root so the shared `core/`
sources are included. If vendoring manually, copy or submodule the whole SDK
repository as `components/honch`.

## Minimal init code

```c
#include "honch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t event_buffer[8192];

#define HONCH_TELEMETRY_TASK_STACK_BYTES 8192

static void honch_telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        honch_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    // ... host-owned esp_netif/event loop/Wi-Fi/time/TLS setup ...

    honch_config_t config = {
        .api_key = "your-api-key",
        .host = "https://i.honch.io",
        .device_model = "my-device",
        .firmware_version = "1.0.0",
        .event_buffer = event_buffer,
        .event_buffer_size = sizeof(event_buffer),
    };

    honch_init(&config);

    xTaskCreate(honch_telemetry_task,
        "honch_telemetry",
        HONCH_TELEMETRY_TASK_STACK_BYTES,
        NULL,
        2,
        NULL);

    // Track custom events
    honch_property_t button_props[] = {
        honch_prop("count", honch_i64(1)),
    };
    honch_track("button_pressed", button_props, 1u);

    // Use sessions to group events
    honch_session_start("user_session");
    honch_property_t screen_props[] = {
        honch_prop("screen", honch_str("home")),
    };
    honch_track("screen_viewed", screen_props, 1u);
    honch_session_end();

}
```

## Run the example app

```bash
cd ports/esp-idf/example
mkdir -p ../local
cp ../local/sdkconfig.defaults.example ../local/sdkconfig.defaults
$EDITOR ../local/sdkconfig.defaults

idf.py menuconfig
# Navigate to "Honch Example Configuration" and set:
#   - Wi-Fi SSID
#   - Wi-Fi Password
#   - Honch API Key
#   - Honch Host (default: https://i.honch.io)

idf.py set-target esp32s3   # or esp32, esp32c3, etc.
idf.py flash monitor
```

`ports/esp-idf/local/sdkconfig.defaults` is ignored by Git and can hold the
Wi-Fi, API key, and host values reused by the ESP-IDF examples and local
sandbox hardware runs. `menuconfig` remains available when you want to override
values for one app build.

## What gets sent automatically

**Auto-stamped properties** (on every event):
- `$device_id` — from config or derived from the ESP MAC address
- `$device_model` — from your config
- `$firmware_version` — from your config
- `$sdk_platform` — `"esp-idf"`
- `$sdk_version` — `"0.2.1"`
- `$environment` — from your config (defaults to `"production"`)
- `$session_id` — only when a session is active
- `$battery_level` — only if you provide a `battery_callback`

**Lifecycle events** (emitted automatically):
- `$device_boot` — on init, with `reset_reason` property
- `$error` — when you call `honch_report_error()`, or when `enable_error_tracking` is true on init after an abnormal ESP reset reason such as panic, watchdog, brownout, or unknown reset
- `$device_shutdown` — on `honch_shutdown()`, followed by a synchronous flush
- `$firmware_update` — on boot if firmware version changed and durable state storage is supplied
- `$battery_low` — when battery drops below threshold (default 15%), emitted once until recovery
- `$session_start` / `$session_end` — when you call the session API

honch init does synchronous work on the caller's task. It validates config,
reads ESP reset/MAC identity data, may call host-supplied state storage for
identity and firmware-version state, and queues `$device_boot` before returning.
It does not perform network I/O; delivery remains cooperative through
`honch_tick()` or explicit flush calls.

Call `honch_report_error()` from normal task context to queue runtime `$error`
events. It follows the same queue and flush policy as `honch_track()` and does
not perform immediate network I/O.

Automatic `$error` capture support is compiled in by default, but boot fault
capture is disabled unless `enable_error_tracking` is true. When enabled, the
SDK maps panic, interrupt/task/other watchdog, brownout, and unknown reset
reasons into a bounded `$error` event with `source`, `severity`, and
`reset_reason` properties during `honch_init()`.
When `enable_crash_symbolication` is also true, ESP-IDF flash ELF coredump
summary metadata is enabled, and ESP-IDF exposes a summary for the previous
crash, the event may also include `crash_summary_version`,
`firmware_build_id`, raw `fault_pc`, raw `backtrace`, and `task_name`. The
shared `$error` contract also supports optional `exception_cause` for ports
that can provide a bounded CPU exception name.
Honch does not save coredumps, collect RAM/register/task snapshots, upload
symbol files, send source code/source paths, or replace ESP-IDF crash forensics
tooling.

The automatic `$error` event is best-effort. If the SDK cannot enqueue it during
startup because the local queue or storage is full, `honch_init()` still
completes so diagnostics do not prevent the device application from starting.

`firmware_build_id` is intended to let backend symbolication match raw crash
addresses to the exact firmware image. Customers should keep symbol upload and
address symbolication as a server-side opt-in workflow.

Error tracking is also build-strip modular. `CONFIG_HONCH_ERROR_TRACKING` and
`CONFIG_HONCH_CRASH_SYMBOLICATION` default to enabled so the feature is
available without extra Kconfig work. Disable `CONFIG_HONCH_ERROR_TRACKING` to
compile out Honch's `$error` emission path. Disable
`CONFIG_HONCH_CRASH_SYMBOLICATION` to keep automatic reset/error events but
remove raw fault-address collection and the ESP-IDF `espcoredump` component
dependency.

## Configuration options

| Field                    | Required | Default        | Description                                |
| ------------------------ | -------- | -------------- | ------------------------------------------ |
| `api_key`                | Yes      | —              | Your Honch project API key                 |
| `host`                   | Yes      | —              | Capture endpoint URL                       |
| `device_model`           | Yes      | —              | Hardware model identifier                  |
| `firmware_version`       | Yes      | —              | Current firmware version string            |
| `environment`            | No       | `"production"` | Environment tag                            |
| `event_buffer`           | Yes      | —              | Caller-owned buffer for queue staging      |
| `event_buffer_size`      | Yes      | —              | Size of the buffer (recommend >= 8192)     |
| `flush_interval_seconds` | No       | 120            | How often to flush events                  |
| `flush_min_interval_ms`  | No       | 15000          | Minimum spacing between outbound uploads   |
| `flush_event_threshold`  | No       | 20             | Flush when this many events are queued     |
| `flush_max_batches`      | No       | 1              | Max batches sent by one `honch_flush()`    |
| `shutdown_flush_max_batches` | No   | 1              | Max batches sent during `honch_shutdown()` |
| `transport_timeout_ms`   | No       | 2500           | Per HTTP request timeout, max 10000 ms     |
| `battery_callback`       | No       | NULL           | Function returning 0-100 or -1             |
| `battery_low_threshold`  | No       | 15             | Battery level that triggers `$battery_low` |
| `enable_error_tracking`  | No       | false          | Emit automatic `$error` after abnormal reset |
| `enable_crash_symbolication` | No   | false          | Include raw build/address context for symbolication when error tracking is enabled and ESP-IDF coredump summary metadata is available |
| `connectivity_callback`  | No       | NULL           | Return false while offline or radio is off |
| `state_storage_ops`      | No       | NULL           | Durable state storage for identity/version |
| `event_queue_ops`        | No       | NULL           | Durable/custom event queue implementation  |

Call `honch_tick()` periodically from a low-priority telemetry task. The SDK
flushes after `flush_interval_seconds` or when the queue reaches
`flush_event_threshold`, and the flush path can perform blocking network I/O.
Do not call `honch_tick()` or `honch_flush()` from an ISR, control loop, UI
loop, high-priority task, ISR-adjacent callback, watchdog-sensitive section, or
other customer-critical path. Keep `honch_track()` on product paths and run
delivery from a background task that your firmware can afford to block for up to
`transport_timeout_ms`; configured values above the hard maximum of 10000 ms are
clamped.

Do not call `honch_tick()` while connectivity is unavailable or the radio is
intentionally off. If that is hard to guarantee, provide
`connectivity_callback`; it must return quickly and should read host-owned
network/radio state. When it returns false, `honch_tick()` keeps queued uploads
pending and `honch_flush()` returns `HONCH_ERR_OFFLINE` without DNS, TLS, retry
backoff growth, or transport I/O.

Successful outbound uploads are spaced by `flush_min_interval_ms`, which
defaults to 15000 ms so telemetry does not continuously share the radio with
product traffic. If events remain queued during the quiet window, `honch_tick()`
keeps the flush request pending and sends later. For benchmark, factory, or
explicit high-throughput modes, set `flush_min_interval_ms` to
`HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS`.

`honch_flush()` is synchronous and bounded by `flush_max_batches`. By default it
sends at most one compact batch per call; additional events may remain queued
for later ticks or explicit flush calls. It also honors `flush_min_interval_ms`;
when called during the quiet window it returns `HONCH_ERR_TRANSPORT` without
sleeping or performing network I/O. Use it only for
explicit maintenance moments where blocking is acceptable.

`honch_shutdown()` emits `$device_shutdown` and then performs a synchronous
flush bounded by `shutdown_flush_max_batches`. By default shutdown sends at most
one compact batch and then completes; unsent events may remain queued and, with
the default RAM queue, are lost when the device resets or powers off.

The ESP-IDF port does not start an SDK-owned worker task. Use an
application-owned FreeRTOS task so task priority, stack size, CPU affinity,
watchdog policy, and shutdown ordering stay under firmware control. A future
SDK-owned worker task should be opt-in only, after flush and shutdown drain
budgets are explicit.

ESP-IDF `xTaskCreate()` stack sizes are in bytes, not FreeRTOS stack words.
Because `honch_tick()` performs HTTPS uploads on the caller's task, the TLS
handshake and POST path need at least 8192 bytes for the Honch telemetry task
unless your firmware has measured a smaller board-specific limit.

## GPIO tracking

The core ESP-IDF SDK does not configure GPIO pins, install the shared GPIO ISR
service, register GPIO ISR handlers, or spawn GPIO worker tasks. GPIO ownership
stays with your firmware because pin mode, pull configuration, interrupt type,
debounce, ISR service ownership, task priority, and shutdown ordering are
product-specific.

To track button or GPIO events, handle GPIO in your own ISR handoff path and
call `honch_track()` from normal task context:

```c
const honch_property_t props[] = {
    honch_prop("pin", honch_i64(0)),
};
honch_track("button_pressed", props, 1u);
```

See `ports/esp-idf/example_gpio` for a host-owned GPIO example.

Flush sends compact chunk wire frames to `POST /capture` with
`Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and
`X-Honch-Stream-Id`.

Use HTTPS in production. Verify device time and certificate trust before
debugging event encoding. Do not disable certificate verification in production.
Honch does not call `esp_netif_init()` or
`esp_event_loop_create_default()` during `honch_init()`; those process-global
networking primitives stay owned by the host firmware.

## Queue storage policy

The public `honch_init()` path queues events in the caller-provided RAM buffer,
then flushes them in compact batches. When the buffer is full, the SDK drops the
oldest queued events to make room for newer telemetry. Oversized events are
rejected.

This keeps tracking calls fast and avoids hidden flash writes. The tradeoff is
explicit: queued events, `identify()` state, and firmware-version state are
volatile unless you provide custom storage. Use `event_queue_ops` for a durable
event queue and `state_storage_ops` for durable identity/version state. Those
hooks can be backed by NVS, a filesystem, external flash, FRAM, or a product
specific queue.

After `identify()`, future events use the new distinct ID. The `$identify`
event also includes the previous identity as `$anon_distinct_id`, usually the
device ID, so earlier anonymous events can merge into the identified person.

The default RAM queue is bounded and compact. Consuming a non-tail event uses an
O(n) memmove per consumed event to keep the caller-provided buffer contiguous;
this is acceptable at the default sizes, but larger custom queue limits should
be measured on the target board.

## Troubleshooting

**Events not appearing in the dashboard?**
- Run `idf.py monitor` and look for `[honch]` log lines
- Check that you see "Batch sent successfully" — if not, events are queuing locally

**HTTP 401 errors?**
- Your API key is invalid. Double-check the key in your config.

**Events queuing but never sending?**
- Verify Wi-Fi is connected (look for "Got IP" in logs)
- The SDK waits for an IP address before attempting any flushes
- Capture must support `POST /capture` with `Content-Type:
  application/vnd.honch.chunk`
- With the default RAM-only queue, unsent events are lost across reset or power
  loss. Use `event_queue_ops` when reboot durability matters more than enqueue
  latency.

## License

Apache 2.0
