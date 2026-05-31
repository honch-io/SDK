# Honch ESP-IDF Footprint

This app measures Honch SDK footprint numbers for release evidence. GPIO
tracking is not part of the core ESP-IDF SDK; measure host-owned GPIO adapter
code in the application that implements it.

## Current ESP32 Build-Size Numbers

Build-size numbers were measured for ESP32 with ESP-IDF v6.0.1 using the
default footprint configuration.

| Metric | Current value | Public claim ceiling |
| --- | ---: | --- |
| Linked Honch SDK code/data (`libhonch.a`) | 32,230 bytes | `<32 KB flash` |
| Honch static RAM (`DRAM + IRAM`) | 621 bytes | `<1 KB static RAM` |

Do not market the whole app delta as the SDK size. From a minimal ESP-IDF app,
the default Honch footprint app adds 419,632 bytes because it pulls in normal ESP-IDF
networking, TLS, HTTP, and Wi-Fi dependencies. That is a worst-case
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
