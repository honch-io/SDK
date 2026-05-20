# Honch ESP-IDF Benchtest

Real-board benchmark app for measuring Honch SDK overhead on ESP-IDF.

It connects to Wi-Fi, synchronizes SNTP time, initializes Honch, then emits
`bench_event` events while logging timing and resource summaries with `BENCH_*`
markers.

## Configure

```bash
cd SDK/ports/esp-idf/benchtest
idf.py menuconfig
```

Set:

- `Wi-Fi SSID`
- `Wi-Fi Password`
- `Honch API Key`
- `Honch Host`

For the local sandbox on the same LAN, use a host like:

```text
http://192.168.1.95:8001
```

## Run

```bash
idf.py set-target esp32
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

Useful monitor lines start with:

```text
BENCH_RUN_START
BENCH_TRACK_SUMMARY
BENCH_FLUSH_SUMMARY
BENCH_RESOURCE_SUMMARY
BENCH_RUN_END
```

Use `Ctrl+]` to exit the ESP-IDF monitor.
