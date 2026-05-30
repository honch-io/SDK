# Honch ESP-IDF SDK

Product analytics for connected hardware. Drop this component into your ESP-IDF project and get device lifecycle events, custom tracking, and GPIO-triggered events flowing to Honch in minutes.

Events are queued locally and sent to Capture as compact chunk wire frames.
The default ESP-IDF integration uses a RAM-first queue for low `honch_track()`
latency. NVS is used for overflow, while permanently rejected events are dropped
instead of dead-lettered to protect the shared Wi-Fi/NVS partition.

## Requirements

- ESP-IDF >= 5.0
- An ESP32 dev board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- A Honch API key (get one from your project settings at [honch.io](https://honch.io))

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

static uint8_t event_buffer[8192];

void app_main(void)
{
    // ... Wi-Fi + NVS init ...

    honch_config_t config = {
        .api_key = "your-api-key",
        .host = "https://capture.honch.io",
        .device_model = "my-device",
        .firmware_version = "1.0.0",
        .event_buffer = event_buffer,
        .event_buffer_size = sizeof(event_buffer),
    };

    honch_init(&config);

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
- `$device_id` — stable generated identifier persisted by the SDK
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
- `$firmware_update` — on boot if firmware version changed, with `previous_version` and `new_version`
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
| `battery_callback`       | No       | NULL           | Function returning 0-100 or -1             |
| `battery_low_threshold`  | No       | 15             | Battery level that triggers `$battery_low` |
| `durability_mode`        | No       | `HONCH_DURABILITY_OS_BUFFERED` | Queue write durability mode |

Call `honch_tick()` periodically from your main loop or scheduler. The SDK
flushes after `flush_interval_seconds` or when the queue reaches
`flush_event_threshold`.

Flush sends compact chunk wire frames to `POST /capture` with
`Content-Type: application/vnd.honch.chunk`, `X-Honch-Project-Key`, and
`X-Honch-Stream-Id`. Capture also accepts the same format on `/e` and
`/chunks`.

## Queue storage policy

The public `honch_init()` path is optimized for device hot paths: new events are
queued into the caller-provided RAM buffer first, then flushed in compact batches.
This keeps tracking calls fast and avoids NVS writes for every event.

Tradeoff: events still in RAM can be lost on reset or power loss. NVS remains in
the storage adapter for overflow, and any NVS-backed events already present at
boot are drained before the SDK returns to RAM-only operation. Permanent
rejections are discarded on ESP-IDF because persisting unused dead-letter
payloads can exhaust the small default NVS partition shared with Wi-Fi
calibration data.

If your product needs stricter reboot durability for NVS-backed queue writes, set
`durability_mode` to `HONCH_DURABILITY_SYNC_ALWAYS`. The default
`HONCH_DURABILITY_OS_BUFFERED` mode favors normal platform buffering. With the
default public init path, new events still enter the caller-provided RAM queue
first; strict durability only applies once events use the persistent storage
adapter.

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
- With the default RAM-first queue, unsent events may be lost across reset or
  power loss. Use an NVS-only storage integration when reboot durability matters
  more than enqueue latency.

**NVS errors at init?**
- Make sure you call `nvs_flash_init()` before `honch_init()`
- If NVS is corrupted, erase and reinit: `nvs_flash_erase()` then `nvs_flash_init()`

## License

Apache 2.0
