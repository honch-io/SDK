# Honch ESP-IDF SDK

Product analytics for connected hardware. Drop this component into your ESP-IDF project and get device lifecycle events, custom tracking, and GPIO-triggered events flowing to Honch in minutes.

Events are queued as CBOR blobs and sent to Capture as uncompressed `application/cbor` batches.

## Requirements

- ESP-IDF >= 5.0
- An ESP32 dev board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- A Honch API key (get one from your project settings at [honch.io](https://honch.io))

## Add to your project

**Option A: ESP Component Manager**

```
idf.py add-dependency "honch-io/honch^0.1.0"
```

**Option B: Git submodule**

```bash
git submodule add https://github.com/honch-io/honch.git components/honch
```

Or simply copy the `honch/` directory into your project's `components/` folder.

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
    honch_track("button_pressed", "{\"count\": 1}");

    // Use sessions to group events
    honch_session_start("user_session");
    honch_track("screen_viewed", "{\"screen\": \"home\"}");
    honch_session_end();

    // Track GPIO pins (e.g., a button)
    honch_track_gpio(GPIO_NUM_0, "boot_button", HONCH_GPIO_FALLING_EDGE);
}
```

## Run the example app

```bash
cd example
idf.py menuconfig
# Navigate to "Honch Example Configuration" and set:
#   - Wi-Fi SSID
#   - Wi-Fi Password
#   - Honch API Key
#   - Honch Host (default: https://capture.honch.io)

idf.py set-target esp32s3   # or esp32, esp32c3, etc.
idf.py flash monitor
```

## Measure SDK footprint

Use the footprint tool when you need defensible flash, RAM, and CPU numbers for
docs or landing pages.

Current ESP32 measurement, from `footprint/` on ESP-IDF v6.0.1:

- `<20 KB flash` for linked Honch SDK code/data (`11,306` bytes measured)
- `<2 KB static RAM` (`1,748` bytes measured)
- `<15 KB runtime heap after init` (`14,304` bytes measured)
- `<0.09% CPU at 1 event/sec` for `honch_track()` RAM-queue hot path
- `<0.002% CPU at 1 event/min` for the same measured hot path amortized

```bash
cd SDK
source /Users/morgana/.espressif/tools/activate_idf_v6.0.1.sh
./tools/measure_esp_idf_footprint.py --target esp32
```

For runtime heap and CPU/event measurements, flash the Honch-enabled footprint
app and feed the monitor log back into the same tool. See
[`footprint/README.md`](footprint/README.md).

## What gets sent automatically

**Auto-stamped properties** (on every event):
- `$device_id` — stable hardware identifier derived from MAC
- `$device_model` — from your config
- `$firmware_version` — from your config
- `$sdk_platform` — `"esp-idf"`
- `$sdk_version` — `"0.1.0"`
- `$environment` — from your config (defaults to `"production"`)
- `$session_id` — only when a session is active
- `$battery_level` — only if you provide a `battery_callback`
- `$wifi_rssi` — only when Wi-Fi is connected

**Lifecycle events** (emitted automatically):
- `$device_boot` — on init, with `reset_reason` property
- `$device_shutdown` — on `honch_shutdown()`, followed by a synchronous flush
- `$connectivity_change` — on Wi-Fi connect/disconnect, with `state` property
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
| `event_buffer`           | Yes      | —              | Caller-owned buffer for CBOR queue staging |
| `event_buffer_size`      | Yes      | —              | Size of the buffer (recommend >= 8192)     |
| `flush_interval_seconds` | No       | 60             | How often to flush events                  |
| `flush_event_threshold`  | No       | 30             | Flush when this many events are queued     |
| `battery_callback`       | No       | NULL           | Function returning 0-100 or -1             |
| `battery_low_threshold`  | No       | 15             | Battery level that triggers `$battery_low` |

**Kconfig options** (set via `idf.py menuconfig`):
- `CONFIG_HONCH_LOG_VERBOSE` — enable extra debug logging
- `CONFIG_HONCH_MAX_QUEUE_DEPTH` — max events in NVS queue (default 256)
- `CONFIG_HONCH_ENABLE_GZIP` — gzip large HTTP CBOR batches when beneficial (default enabled)
- `CONFIG_HONCH_GZIP_MIN_BYTES` — minimum CBOR payload size before gzip is attempted (default 1024)

## Troubleshooting

**Events not appearing in the dashboard?**
- Run `idf.py monitor` and look for `[honch]` log lines
- Check that you see "Batch sent successfully" — if not, events are queuing locally

**HTTP 401 errors?**
- Your API key is invalid. Double-check the key in your config.

**Events queuing but never sending?**
- Verify Wi-Fi is connected (look for "Got IP" in logs)
- The SDK waits for an IP address before attempting any flushes
- Capture must support the CBOR ingest contract for `POST /batch`

**NVS errors at init?**
- Make sure you call `nvs_flash_init()` before `honch_init()`
- If NVS is corrupted, erase and reinit: `nvs_flash_erase()` then `nvs_flash_init()`

## License

Apache 2.0
