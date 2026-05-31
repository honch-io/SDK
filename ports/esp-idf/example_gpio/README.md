# Honch ESP-IDF GPIO Example

This example shows the host-owned GPIO tracking pattern. The Honch ESP-IDF SDK
does not configure GPIO pins, install the GPIO ISR service, register GPIO ISR
handlers, or start GPIO worker tasks.

The application owns the hardware path:

```text
GPIO interrupt -> app-owned ISR -> app-owned queue -> app-owned task -> honch_track()
```

## Setup

1. Configure via menuconfig:
   ```bash
   idf.py menuconfig
   ```
   Set these under "Honch GPIO Example Configuration":
   - **Wi-Fi SSID**
   - **Wi-Fi Password**
   - **Honch API Key**
   - **Honch Host**
   - **Button GPIO**

2. Build and flash:
   ```bash
   idf.py set-target esp32
   idf.py flash monitor
   ```

3. Press the configured button GPIO. The app-owned GPIO task debounces the edge
   and sends a `button_pressed` event with a `pin` property.

## Ownership Notes

GPIO setup is board- and firmware-specific. Keep pin mode, pulls, interrupt
type, ISR service ownership, task priority, stack size, debounce, and shutdown
ordering in your firmware. Call `honch_track()` only from normal task context,
not directly from an ISR.
