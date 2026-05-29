# Honch ESP-IDF Rate Sweep Benchmark

Dedicated real-board benchmark app for collecting rate-sweep data for the
Honch benchmark site.

This app is separate from `ports/esp-idf/benchtest` so the existing benchtest
and offline queue proof remain unchanged.

## Configure

```bash
cd SDK/ports/esp-idf/rate_sweep_bench
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
idf.py -p /dev/cu.usbserial-0001 flash monitor | tee esp32-rate-sweep.log
```

Useful monitor markers:

```text
BENCH_SUITE_START
BENCH_RATE_START
BENCH_WINDOW
BENCH_RATE_END
BENCH_SUITE_END
```

Each `BENCH_WINDOW` line includes actual EPS, track latency p50/p95/p99, flush
latency, CPU, heap, stack, RSSI, queue estimate, and failure counts. If a target
rate saturates the device, use `actual_eps`, latency, queue growth, and failures
as the benchmark result.
