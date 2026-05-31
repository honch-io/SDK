# Honch ESP-IDF Example

Minimal example that connects to Wi-Fi and sends events to Honch.

## Setup

1. Configure via menuconfig:
   ```bash
   idf.py menuconfig
   ```
   Set these under "Honch Example Configuration":
   - **Wi-Fi SSID** — your network name
   - **Wi-Fi Password** — your network password
   - **Honch API Key** — from your Honch project settings
   - **Honch Host** — defaults to `https://capture.honch.io`

2. Build and flash:
   ```bash
   idf.py set-target esp32s3
   idf.py flash monitor
   ```

3. Watch the example send startup and heartbeat events.

4. Watch events appear in your Honch dashboard.

For GPIO/button tracking, use the separate `ports/esp-idf/example_gpio`
example. The core SDK does not configure GPIO pins or install GPIO ISR
handlers.
