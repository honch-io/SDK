# Honch ESP-IDF Footprint

This app measures landing-page-safe Honch SDK footprint numbers.

## Current ESP32 Numbers

Measured on an ESP32-D0WD-V3 rev 3.1 board with ESP-IDF v6.0.1, default
Honch production config, and `CONFIG_HONCH_PERF_LOGGING` disabled.

| Metric | Current value | Landing-safe claim |
| --- | ---: | --- |
| Linked Honch SDK code/data (`libhonch.a`) | 11,306 bytes | `<20 KB flash` |
| Honch static RAM (`DRAM + IRAM`) | 1,748 bytes | `<2 KB static RAM` |
| Runtime heap after `honch_init()` | 14,304 bytes | `<15 KB heap after init` |
| Runtime heap after representative API setup | 16,740 bytes | `<17 KB heap after setup` |
| `honch_track()` CPU, RAM queue hot path | avg 889 us, p50 873 us, p95 1,037 us | `<0.09% CPU at 1 event/sec` |
| `honch_track()` CPU at 1 event/min | avg 889 us amortized | `<0.002% CPU at 1 event/min` |

Do not market the whole app delta as the SDK size. From a minimal ESP-IDF app,
the Honch footprint app adds 425,378 bytes because it pulls in normal ESP-IDF
networking, TLS, HTTP, Wi-Fi, NVS, and CBOR dependencies. That is a worst-case
"starting from empty firmware" number, not the size of Honch's own code.

## Method

The build-size report combines direct archive attribution and differential
diagnostics:

```text
SDK code/RAM claim = linked esp-idf/honch/libhonch.a contribution
Worst-case dependency pull-in = Honch app - minimal app
Connected baseline delta = diagnostic only when non-negative
```

This avoids inflated or negative claims from ESP-IDF dependency differences.
The connected baseline intentionally remains in the report so regressions are
visible, but it is not used for landing copy when the baseline is larger than
the Honch app.

## Build-Size Report

From the SDK repo root:

```bash
source /Users/morgana/.espressif/tools/activate_idf_v6.0.1.sh
./tools/measure_esp_idf_footprint.py --target esp32
```

The script writes:

```text
ports/esp-idf/footprint/build-footprint-report.json
```

## Runtime Heap and CPU

Flash the Honch-enabled footprint app and capture the monitor output:

```bash
cd ports/esp-idf/footprint
idf.py -B build-footprint-esp32-honch -D IDF_TARGET=esp32 -D HONCH_FOOTPRINT_WITH_SDK=ON -p /dev/cu.usbserial-0001 erase-flash flash monitor | tee footprint-monitor.log
```

Exit the monitor with `Ctrl+]`, then merge the runtime markers into the report:

```bash
cd ../../..
./tools/measure_esp_idf_footprint.py --target esp32 --skip-build --monitor-log ports/esp-idf/footprint/footprint-monitor.log
```

Useful monitor markers:

```text
HONCH_FOOTPRINT_RUNTIME
HONCH_FOOTPRINT_CPU
```
