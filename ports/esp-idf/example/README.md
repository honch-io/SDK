# Honch ESP-IDF Water Filter Example

Realistic ESP-IDF example for a connected water-filter station. It connects to
Wi-Fi, initializes Honch, identifies a facility operator, tracks maintenance and
dispense sessions, and sends periodic device health reports.

## Setup

1. Configure via menuconfig:
   ```bash
   mkdir -p ../local
   cp ../local/sdkconfig.defaults.example ../local/sdkconfig.defaults
   $EDITOR ../local/sdkconfig.defaults

   idf.py menuconfig
   ```
   Set these under "Honch Example Configuration":
   - **Wi-Fi SSID** — your network name
   - **Wi-Fi Password** — your network password
   - **Honch API Key** — from your Honch project settings
   - **Honch Host** — defaults to `https://i.honch.io`

2. Build and flash:
   ```bash
   idf.py set-target esp32s3
   idf.py flash monitor
   ```

3. Watch the example send water-filter station events:
   - device lifecycle events from the SDK
   - `device_provisioned`
   - `filter_status_checked`
   - `filter_replaced`
   - `dispense_started`
   - `dispense_completed`
   - `device_health_report`

4. Watch events appear in your Honch dashboard.

The product-specific flow lives in `main/water_filter.c`. `main/app_main.c`
keeps platform setup separate: Wi-Fi, SNTP, Honch initialization, and the
background telemetry tick task.

For GPIO/button tracking, use the separate `ports/esp-idf/example_gpio`
example. The core SDK does not configure GPIO pins or install GPIO ISR
handlers.
