# ESP-IDF Footprint: main

Branch/worktree: `main` at `f734f68-dirty` in `/private/tmp/honch-sdk-main-bench`

Source report: `/private/tmp/honch-main-footprint-report.json`

## Build Footprint

| Metric | Value |
| --- | ---: |
| Direct `libhonch.a` flash | 11,717 bytes |
| Direct `libhonch.a` static RAM | 1,764 bytes |
| Minimal app image delta | 424,746 bytes |
| Minimal app static RAM delta | 22,766 bytes |
| Connected app image delta | -253,341 bytes |
| Connected app static RAM delta | -17,137 bytes |

Connected-app deltas are diagnostic only because the connected baseline is larger than the Honch app.

## Runtime Footprint

Captured on real ESP32-D0WD-V3 rev 3.1 with the footprint app overlaid onto `main`.

| Phase | Heap Free | Heap Min | Largest Block | Stack High Water |
| --- | ---: | ---: | ---: | ---: |
| boot | 255,056 | 252,328 | 126,976 | 2,548 |
| before_honch | 255,056 | 252,328 | 126,976 | 2,548 |
| after_honch_init | 240,924 | 240,792 | 110,592 | 2,436 |
| after_track_sample | 224,412 | 224,220 | 110,592 | 2,436 |
| after_representative_api | 221,972 | 221,972 | 110,592 | 2,436 |

CPU sample:

| Metric | Value |
| --- | ---: |
| Samples | 32 |
| Failures | 0 |
| Average | 423 us |
| Min | 413 us |
| P50 | 417 us |
| P95 | 533 us |
| Max | 550 us |
| CPU at 1 Hz | 0.042300% |
