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

3. Press the BOOT button on your dev board — each press sends a `button_pressed` event.

4. Watch events appear in your Honch dashboard.
