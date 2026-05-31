# Honch ESP-IDF SDK

Stable ESP-IDF component for Honch product analytics on connected hardware.

Events are queued locally and sent to Capture as compact chunk wire frames.
The default ESP-IDF integration is RAM-only by default for low `honch_track()`
latency. Events are not preserved across reset or power loss unless the
integrator supplies durable queue storage through `event_queue_ops`.

## Status

Stable `0.2.0`.

## Requirements

- ESP-IDF >= 5.0
- An ESP32 dev board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- A Honch project key.
- Wi-Fi, time, and TLS trust configured before expecting HTTPS delivery.

## Add to your project

**Option A: ESP Component Manager**

```
idf.py add-dependency "honch-io/honch^0.2.0"
```

**Option B: Git submodule**

```bash
git submodule add https://github.com/honch-io/honch.git components/honch
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
    // ... Wi-Fi/time/TLS setup ...

    honch_config_t config = {
        .api_key = "your-api-key",
        .host = "https://capture.honch.io",
        .device_model = "my-device",
        .firmware_version = "1.0.0",
        .event_buffer = event_buffer,
        .event_buffer_size = sizeof(event_buffer),
    };

    honch_init(&config);

    xTaskCreate(honch_telemetry_task, "honch_telemetry", 4096, NULL, 2, NULL);

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

    // Track GPIO pins (e.g., a button)
    honch_track_gpio(GPIO_NUM_0, "boot_button", HONCH_GPIO_FALLING_EDGE);
}
```

## Run the example app

```bash
cd ports/esp-idf/example
idf.py menuconfig
# Navigate to "Honch Example Configuration" and set:
#   - Wi-Fi SSID
#   - Wi-Fi Password
#   - Honch API Key
#   - Honch Host (default: https://capture.honch.io)

idf.py set-target esp32s3   # or esp32, esp32c3, etc.
idf.py flash monitor
```

## What gets sent automatically

**Auto-stamped properties** (on every event):
- `$device_id` — from config or derived from the ESP MAC address
- `$device_model` — from your config
- `$firmware_version` — from your config
- `$sdk_platform` — `"esp-idf"`
- `$sdk_version` — `"0.2.0"`
- `$environment` — from your config (defaults to `"production"`)
- `$session_id` — only when a session is active
- `$battery_level` — only if you provide a `battery_callback`

**Lifecycle events** (emitted automatically):
- `$device_boot` — on init, with `reset_reason` property
- `$device_shutdown` — on `honch_shutdown()`, followed by a synchronous flush
- `$firmware_update` — on boot if firmware version changed and durable state storage is supplied
- `$battery_low` — when battery drops below threshold (default 15%), emitted once until recovery
- `$session_start` / `$session_end` — when you call the session API

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
| `flush_interval_seconds` | No       | 60             | How often to flush events                  |
| `flush_event_threshold`  | No       | 30             | Flush when this many events are queued     |
| `transport_timeout_ms`   | No       | 3000           | Per HTTP request timeout                   |
| `battery_callback`       | No       | NULL           | Function returning 0-100 or -1             |
| `battery_low_threshold`  | No       | 15             | Battery level that triggers `$battery_low` |
| `state_storage_ops`      | No       | NULL           | Durable state storage for identity/version |
| `event_queue_ops`        | No       | NULL           | Durable/custom event queue implementation  |

Call `honch_tick()` periodically from a low-priority telemetry task. The SDK
flushes after `flush_interval_seconds` or when the queue reaches
`flush_event_threshold`, and the flush path can perform blocking network I/O.
Do not call `honch_tick()` or `honch_flush()` from an ISR, control loop, UI
loop, or other customer-critical path. Keep `honch_track()` on product paths and
run delivery from a background task that your firmware can afford to block for
up to `transport_timeout_ms`.

`honch_flush()` drains synchronously and can perform multiple HTTP requests.
Use it only for explicit maintenance/shutdown moments where blocking is
acceptable. `honch_shutdown()` emits `$device_shutdown` and then flushes
synchronously, so call it from the same kind of non-critical context.

The ESP-IDF port does not start an SDK-owned worker task. Use an
application-owned FreeRTOS task so task priority, stack size, CPU affinity,
watchdog policy, and shutdown ordering stay under firmware control. A future
SDK-owned worker task should be opt-in only, after flush and shutdown drain
budgets are explicit.

Flush sends compact chunk wire frames to `POST /capture` with
`Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and
`X-Honch-Stream-Id`.

Use HTTPS in production. Verify device time and certificate trust before
debugging event encoding. Do not disable certificate verification in production.

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
