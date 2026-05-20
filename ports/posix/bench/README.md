# Honch C/POSIX Benchmarks

This directory contains the C/POSIX SDK benchmark harness. It is meant to
produce repeatable baseline numbers for SDK overhead before optimizing:

- wall-clock latency for hot SDK operations
- p50/p95/p99 tail latency
- SDK-only allocation counters and peak allocated bytes
- file-backed queue size after each scenario
- fake transport calls, payload bytes, and gzip usage
- process RSS for trend checks

## Build

```sh
cd ports/posix
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DHONCH_BUILD_TESTS=OFF \
  -DHONCH_BUILD_EXAMPLES=OFF \
  -DHONCH_BUILD_BENCHMARKS=ON
cmake --build build-bench --target honch_posix_bench
```

## Run

```sh
./build-bench/bench/honch_posix_bench
```

To save a baseline:

```sh
./build-bench/bench/honch_posix_bench | tee bench/baseline.csv
```

The benchmark prints CSV rows:

```text
name,phase,iterations,events_per_iteration,total_us,mean_us,min_us,p50_us,p95_us,p99_us,max_us,peak_rss_kb,sdk_current_bytes,sdk_peak_bytes,sdk_total_allocated_bytes,sdk_total_freed_bytes,sdk_malloc_calls,sdk_calloc_calls,sdk_realloc_calls,sdk_free_calls,sdk_failed_allocations,sdk_live_allocations,sdk_peak_live_allocations,transport_calls,transport_bytes,transport_max_body_bytes,transport_identity_calls,transport_gzip_calls,queue_pending_files,queue_pending_bytes,queue_dead_files,queue_dead_bytes,status
track_small_properties,track,1000,1,...
```

## What The Numbers Mean

- `phase`: measured operation type, such as `track`, `flush`, or full
  lifecycle.
- `iterations`: number of measured operations.
- `events_per_iteration`: event count handled by each measured operation.
- `mean_us`, `min_us`, `p50_us`, `p95_us`, `p99_us`, `max_us`: wall-clock
  latency for the measured operation.
- `peak_rss_kb`: process peak resident set size reported by `getrusage`. This
  includes process/runtime overhead, so use it for trends, not SDK-only claims.
- `sdk_current_bytes`: SDK allocation bytes still live at the point the row is
  reported. Track/flush rows are reported before client shutdown, so some live
  client state is expected.
- `sdk_peak_bytes`: peak SDK allocation bytes observed during the scenario.
- `sdk_total_allocated_bytes` / `sdk_total_freed_bytes`: cumulative SDK heap
  traffic. High totals with low peak bytes usually mean churn, not leaks.
- `sdk_malloc_calls`, `sdk_calloc_calls`, `sdk_realloc_calls`,
  `sdk_free_calls`: SDK allocation call counts.
- `sdk_failed_allocations`: allocation failures during the scenario. Expected
  value is `0`.
- `sdk_live_allocations`, `sdk_peak_live_allocations`: live allocation object
  counts at row time and peak.
- `transport_calls`: fake transport calls made by the SDK.
- `transport_bytes`: bytes handed to the fake transport.
- `transport_max_body_bytes`: largest single fake transport body.
- `transport_identity_calls`: fake transport calls without gzip.
- `transport_gzip_calls`: fake transport calls with `Content-Encoding: gzip`.
- `queue_pending_files` / `queue_pending_bytes`: queued events left pending
  after the measured operation.
- `queue_dead_files` / `queue_dead_bytes`: events moved to the dead-letter
  directory after permanent rejection.
- `status`: SDK status for the measured operation.

The fake transport removes real network latency so the result is SDK encode,
queue, batch, compression, filesystem, and scheduling overhead. For consumer
product claims, run this on the same class of hardware and filesystem as the
target product, then compare Release builds across commits.

## Current Scenarios

- `init_shutdown`: full init, boot event, shutdown event, flush, teardown.
- `track_empty_properties`: queue one event with `{}`.
- `track_small_properties`: queue one small event.
- `track_nested_properties`: queue nested map/array/float/bool properties.
- `track_1kb_properties`: queue a larger payload.
- `flush_1_raw_success`: flush one queued event without gzip.
- `flush_50_raw_success`: flush 50 queued events without gzip.
- `flush_200_raw_success`: flush 200 queued events without gzip.
- `flush_50_gzip_success`: flush 50 queued events with gzip enabled.
- `flush_50_retry_500`: server error path; events should remain pending.
- `flush_50_rejected_400`: permanent rejection path; events should move to
  dead-letter storage.

## Suggested Baselines

Run these before and after optimization work:

```sh
./build-bench/bench/honch_posix_bench | tee bench/baseline.csv
size build-bench/libhonch_posix.a
```

For a more complete local snapshot:

```sh
otool -L build-bench/bench/honch_posix_bench
/usr/bin/time -l ./build-bench/bench/honch_posix_bench > bench/time-baseline.csv
```

On Linux, add per-symbol size and heap profiling:

```sh
nm -S --size-sort build-bench/libhonch_posix.a | tail -40
valgrind --tool=massif --massif-out-file=bench/massif.out ./build-bench/bench/honch_posix_bench
ms_print bench/massif.out > bench/massif.txt
```

On macOS, `nm -S` reports zero sizes for Mach-O archive members. Use `size` for
object-level footprint and Instruments Allocations or Leaks against
`./build-bench/bench/honch_posix_bench` for runtime profiling.
