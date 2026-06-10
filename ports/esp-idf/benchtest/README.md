# Honch ESP-IDF Benchtest

Real-board benchmark app for measuring Honch SDK overhead on ESP-IDF.

It connects to Wi-Fi, synchronizes SNTP time, initializes Honch, then emits
`bench_event` events while logging timing and resource summaries with `BENCH_*`
markers.

## Configure

```bash
cd SDK/ports/esp-idf/benchtest
mkdir -p ../local
cp ../local/sdkconfig.defaults.example ../local/sdkconfig.defaults
$EDITOR ../local/sdkconfig.defaults

idf.py menuconfig
```

Set:

- `Wi-Fi SSID`
- `Wi-Fi Password`
- `Honch API Key`
- `Honch Host`

For the local sandbox on the same LAN, use a host like:

```text
http://192.168.1.122:8001
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

## Offline Queue Proof

Enable `BENCH_OFFLINE_QUEUE_PROOF` when you need to prove ESP32 queue
preservation through a capture outage.

Expected flow:

1. Stop capture or block the configured `Honch Host`.
2. Flash/monitor the benchtest firmware with `BENCH_OFFLINE_QUEUE_PROOF=y`.
3. Wait for `BENCH_OFFLINE_QUEUE_GROWTH queued_estimate=<nonzero>`.
4. Restore capture.
5. Wait for `BENCH_OFFLINE_RECOVERY_FLUSH queued_estimate=0`.
6. Verify `bench_offline_queue_proof` rows for the logged `proof_id` in
   your Capture backend.

Proof-specific monitor markers:

```text
BENCH_OFFLINE_PROOF_START
BENCH_OFFLINE_QUEUE_GROWTH
BENCH_OFFLINE_RECOVERY_WAIT
BENCH_OFFLINE_RECOVERY_FLUSH
BENCH_OFFLINE_PROOF_DONE
```
