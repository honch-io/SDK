# ESP-IDF Footprint: refactor

Branch/worktree: `refactor/c-core-canonical` at `58a8a37-dirty`

Source report: `/private/tmp/honch-refactor-footprint-after-ramfix.json`

## Build Footprint

| Metric | Value |
| --- | ---: |
| Direct `libhonch.a` flash | 21,720 bytes |
| Direct `libhonch.a` static RAM | 1,303 bytes |
| Minimal app image delta | 416,968 bytes |
| Minimal app static RAM delta | 38,904 bytes |
| Connected app image delta | -261,119 bytes |
| Connected app static RAM delta | -25,519 bytes |

Connected-app deltas are diagnostic only because the connected baseline is larger than the Honch app.

## Runtime Footprint

Captured on real ESP32-D0WD-V3 rev 3.1 from `ports/esp-idf/footprint`.

| Phase | Heap Free | Heap Min | Largest Block | Stack High Water |
| --- | ---: | ---: | ---: | ---: |
| boot | 264,100 | 261,372 | 131,072 | 2,648 |
| before_honch | 264,100 | 261,372 | 131,072 | 2,648 |
| after_honch_init | 258,800 | 258,800 | 126,976 | 2,328 |
| after_track_sample | 258,800 | 257,736 | 126,976 | 2,328 |
| after_representative_api | 258,436 | 257,636 | 126,976 | 2,328 |

CPU sample:

| Metric | Value |
| --- | ---: |
| Samples | 32 |
| Failures | 0 |
| Average | 230 us |
| Min | 219 us |
| P50 | 220 us |
| P95 | 231 us |
| Max | 564 us |
| CPU at 1 Hz | 0.023000% |
