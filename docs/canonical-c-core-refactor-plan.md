# Canonical C Core SDK Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the SDK repository so one portable C core owns Honch SDK behavior, while ESP-IDF, POSIX, MicroPython, and the planned React Native relay path become ports, adapters, or conformance-compatible wrappers.

**Architecture:** Split today's mixed C/POSIX implementation into a platform-neutral core plus platform ports. The core owns event semantics, CBOR encoding, identity/session behavior, queue policy, retry classification, and relay packetization; ports provide storage, transport, time, random, locking, scheduling, and platform metadata.

**Tech Stack:** C11, CMake, ESP-IDF, FreeRTOS, POSIX/pthreads/libcurl, MicroPython, CBOR, gzip, Honch capture ingest, future BLE/mobile relay.

---

## Why This Refactor Exists

The current SDK repo has three real SDK implementations:

- `esp-idf/`: C component with ESP-IDF-specific queue, scheduler, transport, identity, lifecycle, and GPIO code.
- `c-core/`: C/POSIX SDK that mixes reusable SDK behavior with POSIX files, pthreads, and libcurl.
- `micropython/`: Python implementation of the same contract with its own queue, scheduler, identity, transport, and encoder.

That shape is already creating duplicated behavior. Queue policy, retry semantics, identity lifecycle, CBOR encoding, gzip behavior, and lifecycle events must be fixed independently across SDKs.

The Memfault SDK in `../memfault-firmware-sdk` shows a better embedded SDK shape:

- `components/` contain reusable modules.
- `ports/` bind those modules to specific platforms.
- A packetizer drains multiple data sources into transport-neutral chunks.
- A port supplies platform storage, time, locks, reboot info, HTTP, timers, and optional metrics.

Honch should copy the shape, not the weight. We do not need Memfault's full diagnostics stack, but we do need one canonical SDK engine.

## Design Principles

1. **One behavior owner.** Core owns SDK semantics. Ports may adapt platform APIs, but must not fork identity, queue, retry, or encoding behavior.
2. **Public compatibility where possible.** ESP-IDF customers should still call `honch_init`, `honch_track`, `honch_flush`, and existing lifecycle APIs.
3. **Transport-neutral data movement.** HTTP is one transport, not the SDK's core assumption. BLE relay, gateway relay, and mobile forwarding use the same packetizer output.
4. **Peek/confirm queue semantics.** Queued events remain pending until a transport or relay path confirms delivery. Avoid pop/requeue as the canonical model.
5. **Small default footprint.** The default core includes event tracking, local queue policy, identity, CBOR, and packetizer. Battery, connectivity, GPIO, logs, and diagnostics remain optional modules or adapters.
6. **Conformance before broad refactor.** Preserve current behavior by writing tests and fixtures before moving implementation.
7. **No platform imports in core.** Core must not include ESP-IDF, FreeRTOS, pthread, curl, filesystem, NVS, MicroPython, React Native, or mobile APIs.

## Current-State Evidence

### Honch SDK

- `c-core/CMakeLists.txt` currently builds one library from reusable files and POSIX-specific files together:
  - reusable-looking: `honch.c`, `honch_cbor.c`, `honch_encoder.c`, `honch_json.c`
  - platform-specific: `honch_platform.c`, `honch_queue.c`, `honch_state.c`, `honch_transport_curl.c`
- `esp-idf/honch/CMakeLists.txt` builds a separate implementation from `honch.c`, `identity.c`, `queue.c`, `encoder.c`, `transport.c`, `scheduler.c`, `gpio.c`, and `lifecycle.c`.
- `micropython/honch/` repeats the same concepts in Python: queue, client, config, encoder, identity, platform, scheduler, transport, and validation.
- `spec/relay-envelope.md` reserves a relay/gateway concept but does not define a working chunk protocol.

### Memfault SDK

- `components/core/src/memfault_data_packetizer.c` centralizes transport-neutral packetization across data sources.
- `components/include/memfault/core/data_packetizer_source.h` defines a data source interface with `has_more_msgs_cb`, `read_msg_cb`, and `mark_msg_read_cb`.
- `components/include/memfault/core/event_storage.h` states event storage is RAM-backed for low latency and can optionally persist to non-volatile storage.
- `components/include/memfault/core/platform/nonvolatile_event_storage.h` defines the optional non-volatile storage vtable.
- `components/include/memfault/util/chunk_transport.h` defines a chunk layer for transports with small MTUs.
- `ports/templates/memfault_platform_port.c` shows the port responsibility: device info, reboot, time, storage boot, metrics boot, logging, and platform initialization.

## Target Repository Layout

```text
SDK/
  core/
    CMakeLists.txt
    include/honch/core/honch.h
    include/honch/core/platform.h
    include/honch/core/storage.h
    include/honch/core/transport.h
    include/honch/core/packetizer.h
    include/honch/core/status.h
    include/honch/core/config.h
    src/honch_core.c
    src/honch_cbor.c
    src/honch_encoder.c
    src/honch_json.c
    src/honch_identity.c
    src/honch_lifecycle.c
    src/honch_queue_policy.c
    src/honch_packetizer.c
    src/honch_retry.c
    src/honch_gzip.c
    test/
      test_core.c
      test_packetizer.c
      test_storage_contract.c
      test_retry.c
      test_conformance.c

  ports/
    posix/
      CMakeLists.txt
      include/honch/posix/honch.h
      src/posix_platform.c
      src/posix_storage.c
      src/posix_transport_curl.c
      src/posix_scheduler.c
      src/posix_compat.c
      test/
        test_posix_storage.c
        test_posix_sdk.c
    esp-idf/
      honch/
        CMakeLists.txt
        Kconfig
        idf_component.yml
        include/honch.h
        src/esp_platform.c
        src/esp_storage_nvs.c
        src/esp_transport_http.c
        src/esp_scheduler.c
        src/esp_lifecycle.c
        src/esp_gpio_adapter.c
        src/esp_compat.c
      example/
      benchtest/
      tests/
    micropython/
      honch/
      examples/
      tests/
    react-native-relay/
      README.md
      src/
      test/

  spec/
    wire-format.md
    relay-envelope.md
    relay-chunks.md
    conformance/
      events/
      batches/
      relay/
```

The physical move can happen gradually. The final layout is the destination, not a requirement for the first commit.

## Canonical Core API

The C core should expose a portable client API:

```c
typedef struct honch_client honch_client_t;

typedef enum honch_status {
    HONCH_OK = 0,
    HONCH_ERROR_INVALID_ARGUMENT = 1,
    HONCH_ERROR_OUT_OF_MEMORY = 2,
    HONCH_ERROR_IO = 3,
    HONCH_ERROR_TRANSPORT = 4,
    HONCH_ERROR_RATE_LIMITED = 5,
    HONCH_ERROR_SERVER = 6,
    HONCH_ERROR_REJECTED = 7,
    HONCH_ERROR_NOT_INITIALIZED = 8,
    HONCH_ERROR_ALREADY_INITIALIZED = 9,
    HONCH_ERROR_QUEUE_FULL = 10,
    HONCH_ERROR_TIMEOUT = 11,
    HONCH_ERROR_INTERNAL = 12
} honch_status_t;

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config);
honch_status_t honch_core_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_core_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_core_set_property(honch_client_t *client, const char *key, const char *value_json);
honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_core_session_end(honch_client_t *client);
honch_status_t honch_core_flush(honch_client_t *client);
honch_status_t honch_core_reset(honch_client_t *client);
honch_status_t honch_core_shutdown(honch_client_t *client);
const char *honch_core_get_device_id(honch_client_t *client);
honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
```

Ports can wrap these names with platform-native public APIs. ESP-IDF may continue exposing `honch_init` and `honch_err_t`.

## Core Platform Interface

Core receives a platform object during init:

```c
typedef struct honch_platform_ops {
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*uptime_ms)(void *ctx);
    honch_status_t (*random_bytes)(void *ctx, uint8_t *buffer, size_t buffer_size);
    honch_status_t (*lock)(void *ctx);
    honch_status_t (*unlock)(void *ctx);
    void (*log)(void *ctx, honch_log_level_t level, const char *message);
    void *ctx;
} honch_platform_ops_t;
```

Rules:

- `now_ms` returns epoch milliseconds when available. A port without wall-clock time returns uptime-derived milliseconds and marks timestamps as device time if the backend needs that distinction.
- `random_bytes` must be cryptographically strong when the platform provides it; otherwise the port must document entropy limits.
- `lock` and `unlock` may be no-ops for single-threaded ports, but the decision belongs to the port, not core.
- Core never logs secrets or full event payloads.

## Core Storage Interface

Core should use one storage interface for identity state and queued events:

```c
typedef struct honch_storage_reader {
    void *ctx;
    honch_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size);
    size_t total_size;
    uint64_t sequence;
} honch_storage_reader_t;

typedef struct honch_storage_ops {
    honch_status_t (*state_get)(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size);
    honch_status_t (*state_set)(void *ctx, const char *key, const uint8_t *data, size_t data_size);
    honch_status_t (*state_delete)(void *ctx, const char *key);

    honch_status_t (*queue_push)(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence);
    honch_status_t (*queue_peek)(void *ctx, honch_storage_reader_t *reader);
    honch_status_t (*queue_consume)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_dead_letter)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_drop_oldest)(void *ctx);
    honch_status_t (*queue_clear)(void *ctx);
    honch_status_t (*queue_depth)(void *ctx, size_t *depth);
    void *ctx;
} honch_storage_ops_t;
```

Rules:

- Core owns max event size, max queue depth, and drop-oldest policy.
- Storage implements atomicity and durability for its medium.
- `queue_peek` is non-destructive.
- `queue_consume` confirms one queued event after successful transport or relay ACK.
- `queue_dead_letter` keeps rejected events inspectable where the platform supports it.
- A storage implementation may keep a RAM hot path, but must expose canonical pending events through peek/consume.

## Core Transport Interface

HTTP is optional from core's perspective:

```c
typedef enum honch_transport_result {
    HONCH_TRANSPORT_ACCEPTED,
    HONCH_TRANSPORT_RETRY,
    HONCH_TRANSPORT_REJECTED,
    HONCH_TRANSPORT_AUTH_ERROR
} honch_transport_result_t;

typedef struct honch_transport_ops {
    honch_status_t (*post_batch)(
        void *ctx,
        const char *endpoint_url,
        const char *api_key,
        const uint8_t *body,
        size_t body_size,
        const char *content_encoding,
        honch_transport_result_t *result);
    void *ctx;
} honch_transport_ops_t;
```

Rules:

- Ports without direct internet transport set transport ops to `NULL` and use the packetizer API.
- HTTP ports classify `2xx`, `401`, `429`, `5xx`, network failures, and other `4xx` consistently with `spec/wire-format.md`.
- Gzip is a core batch-body transform, but availability is controlled by build flags and port capabilities.

## Packetizer And Relay API

The packetizer is the key to BLE-only devices and companion app relay.

```c
typedef enum honch_data_source_mask {
    HONCH_DATA_SOURCE_EVENTS = 1u << 0,
    HONCH_DATA_SOURCE_LOGS = 1u << 1,
    HONCH_DATA_SOURCE_DIAGNOSTICS = 1u << 2,
    HONCH_DATA_SOURCE_ALL = HONCH_DATA_SOURCE_EVENTS | HONCH_DATA_SOURCE_LOGS | HONCH_DATA_SOURCE_DIAGNOSTICS
} honch_data_source_mask_t;

typedef struct honch_packetizer honch_packetizer_t;

bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(
    honch_packetizer_t *packetizer,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size,
    bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);
```

Initial implementation scope:

- `HONCH_DATA_SOURCE_EVENTS` only.
- A chunk frame small enough for BLE MTUs.
- A frame header with protocol version, source type, sequence number, offset, flags, payload length, and CRC.
- Confirm only after the receiver has durably persisted or successfully forwarded the complete message.
- Abort leaves the queued event pending.

The React Native bridge should consume this packetizer contract rather than inventing a separate queue format.

## Relay Contract

Add `spec/relay-chunks.md` with this concrete protocol:

```text
byte 0      protocol version, initially 1
byte 1      source type, initially 1 for events
byte 2      flags: bit 0 = first chunk, bit 1 = final chunk
byte 3      reserved, must be 0
bytes 4-11  sequence number, uint64 big-endian
bytes 12-15 message offset, uint32 big-endian
bytes 16-17 payload length, uint16 big-endian
bytes 18-19 crc16 of header bytes 0-17 plus payload
bytes 20..  payload
```

Receiver rules:

- Reject frames with unsupported version.
- Reject frames with nonzero reserved byte.
- Reassemble by device identity plus sequence number.
- Accept duplicate chunks idempotently when offset and payload match.
- ACK a message only after all chunks are present and the complete CBOR event or batch is durably stored by the relay.
- Firmware calls `honch_packetizer_confirm` only after receiving that ACK.

## Module Boundaries

### Core Modules

- `honch_core.c`: client lifecycle, config validation, public core API.
- `honch_identity.c`: device ID, distinct ID, identify, reset state machine.
- `honch_lifecycle.c`: device boot, shutdown, firmware update, connectivity, battery low event decisions.
- `honch_encoder.c`: event and batch CBOR encoding.
- `honch_cbor.c`: low-level CBOR writer and validator.
- `honch_json.c`: JSON validation and JSON-to-CBOR conversion.
- `honch_queue_policy.c`: max depth, max event size, drop oldest, dead-letter policy.
- `honch_packetizer.c`: chunked relay export and confirm/abort behavior.
- `honch_retry.c`: HTTP response and retry/backoff classification.
- `honch_gzip.c`: optional gzip body transform.

### Port Modules

- POSIX:
  - `posix_storage.c`: filesystem state and pending/dead event queue.
  - `posix_transport_curl.c`: libcurl HTTP transport.
  - `posix_scheduler.c`: pthread background flush.
  - `posix_platform.c`: time, random, locks, logging.
  - `posix_compat.c`: preserves current `honch_init` API.
- ESP-IDF:
  - `esp_storage_nvs.c`: NVS or dedicated partition storage.
  - `esp_transport_http.c`: `esp_http_client` transport.
  - `esp_scheduler.c`: FreeRTOS worker, interval flush, threshold flush.
  - `esp_platform.c`: `esp_timer`, `esp_random`, mutexes, logging, MAC-derived device ID seed.
  - `esp_lifecycle.c`: Wi-Fi connectivity hooks and reset reason adapter.
  - `esp_gpio_adapter.c`: optional GPIO event adapter that calls core track.
  - `esp_compat.c`: preserves current ESP-IDF public API.
- MicroPython:
  - Keep Python implementation initially, but make it pass conformance fixtures generated by core.
  - Later choose between native binding, pure-Python wrapper, or contract-compatible implementation.
- React Native relay:
  - Mobile-side relay queue and uploader.
  - BLE frame receiver using `spec/relay-chunks.md`.
  - Persistent phone backlog before upload.

## Migration Strategy

This is a large refactor. Do it as a series of reviewable PRs on a long-running branch, with each PR preserving a working SDK.

### Milestone 1: Contract And Conformance

Outcome: current behavior is locked before files move.

### Task 1: Add Core Architecture Decision Record

**Files:**
- Create: `docs/adr/0001-canonical-c-core.md`
- Modify: `README.md`

- [ ] **Step 1: Create the ADR**

Add `docs/adr/0001-canonical-c-core.md`:

```markdown
# ADR 0001: Canonical C Core And Platform Ports

## Status

Accepted

## Context

The SDK repository currently contains separate ESP-IDF, C/POSIX, and MicroPython implementations. They duplicate queue, retry, identity, lifecycle, and CBOR behavior. The SDK must support direct HTTP devices and BLE-only devices relayed through companion apps.

## Decision

Honch SDK behavior will be owned by a canonical portable C core. Platform SDKs will be ports or conformance-compatible wrappers. The core owns event semantics, identity, sessions, queue policy, CBOR, retry classification, and packetization. Ports own storage, time, random, locking, scheduling, transport, and platform metadata.

## Consequences

ESP-IDF and POSIX remain public SDKs, but their internals move behind port adapters. MicroPython remains a Python implementation initially, validated by conformance fixtures. React Native relay consumes the packetizer/relay contract.
```

- [ ] **Step 2: Link the ADR from the repo README**

Modify `README.md` under the spec section:

```markdown
- [Canonical C Core ADR](docs/adr/0001-canonical-c-core.md) — SDK architecture direction
```

- [ ] **Step 3: Verify markdown paths**

Run:

```sh
test -f docs/adr/0001-canonical-c-core.md
test -f README.md
```

Expected: both commands exit 0.

- [ ] **Step 4: Commit**

```sh
git add README.md docs/adr/0001-canonical-c-core.md
git commit -m "docs: record canonical c core architecture"
```

### Task 2: Add Conformance Fixture Directory

**Files:**
- Create: `spec/conformance/README.md`
- Create: `spec/conformance/events/basic-track.json`
- Create: `spec/conformance/events/session-track.json`
- Create: `spec/conformance/events/identity-reset.json`
- Create: `spec/conformance/http/response-policy.json`

- [ ] **Step 1: Create the conformance README**

Add `spec/conformance/README.md`:

```markdown
# SDK Conformance Fixtures

These fixtures define behavior every Honch SDK must implement. They are not tied to one language or platform.

## Fixture Types

- `events/`: event-level behavior, identity, sessions, properties, and lifecycle.
- `http/`: response-code classification and retry/drop behavior.
- `relay/`: packetizer and relay chunk behavior.

## Rules

- SDK-owned properties win over user-supplied properties.
- Event timestamps are assigned when `track` is called.
- Queue entries remain pending until delivery is confirmed.
- Retryable failures preserve events.
- Permanent rejection dead-letters or drops according to platform capability.
```

- [ ] **Step 2: Add basic track fixture**

Add `spec/conformance/events/basic-track.json`:

```json
{
  "name": "basic track",
  "config": {
    "api_key": "test-key",
    "device_id": "dev_fixture",
    "device_model": "FixtureBoard",
    "firmware_version": "1.2.3",
    "environment": "test"
  },
  "operations": [
    {
      "op": "track",
      "event": "button_pressed",
      "properties": {
        "pin": 0,
        "$device_id": "spoofed"
      }
    }
  ],
  "expect": {
    "event": "button_pressed",
    "distinct_id": "dev_fixture",
    "properties": {
      "pin": 0,
      "$device_id": "dev_fixture",
      "$device_model": "FixtureBoard",
      "$firmware_version": "1.2.3",
      "$environment": "test"
    },
    "properties_absent": [
      "spoofed"
    ]
  }
}
```

- [ ] **Step 3: Add session fixture**

Add `spec/conformance/events/session-track.json`:

```json
{
  "name": "session track",
  "config": {
    "api_key": "test-key",
    "device_id": "dev_fixture",
    "device_model": "FixtureBoard",
    "firmware_version": "1.2.3",
    "environment": "test"
  },
  "operations": [
    {
      "op": "session_start",
      "session_name": "recording"
    },
    {
      "op": "track",
      "event": "recording_started",
      "properties": {
        "mode": "hdr"
      }
    },
    {
      "op": "session_end"
    }
  ],
  "expect": {
    "events": [
      "$session_start",
      "recording_started",
      "$session_end"
    ],
    "session_id_prefix": "sess_",
    "session_id_present_on": [
      "$session_start",
      "recording_started",
      "$session_end"
    ]
  }
}
```

- [ ] **Step 4: Add reset fixture**

Add `spec/conformance/events/identity-reset.json`:

```json
{
  "name": "identity reset",
  "config": {
    "api_key": "test-key",
    "device_id": "dev_fixture",
    "device_model": "FixtureBoard",
    "firmware_version": "1.2.3",
    "environment": "test"
  },
  "operations": [
    {
      "op": "identify",
      "distinct_id": "user_123",
      "traits": {
        "plan": "beta"
      }
    },
    {
      "op": "reset"
    },
    {
      "op": "track",
      "event": "after_reset",
      "properties": {}
    }
  ],
  "expect": {
    "final_distinct_id": "dev_fixture",
    "queued_events_after_reset": [
      "after_reset"
    ]
  }
}
```

- [ ] **Step 5: Add HTTP response policy fixture**

Add `spec/conformance/http/response-policy.json`:

```json
{
  "name": "http response policy",
  "cases": [
    { "status": 200, "result": "accepted", "queue": "consume" },
    { "status": 202, "result": "accepted", "queue": "consume" },
    { "status": 401, "result": "auth_error", "queue": "drop_or_dead_letter" },
    { "status": 400, "result": "rejected", "queue": "drop_or_dead_letter" },
    { "status": 404, "result": "rejected", "queue": "drop_or_dead_letter" },
    { "status": 429, "result": "retry", "queue": "preserve" },
    { "status": 500, "result": "retry", "queue": "preserve" },
    { "status": 503, "result": "retry", "queue": "preserve" },
    { "status": 0, "result": "retry", "queue": "preserve" }
  ]
}
```

- [ ] **Step 6: Validate JSON fixtures**

Run:

```sh
python3 -m json.tool spec/conformance/events/basic-track.json
python3 -m json.tool spec/conformance/events/session-track.json
python3 -m json.tool spec/conformance/events/identity-reset.json
python3 -m json.tool spec/conformance/http/response-policy.json
```

Expected: each command prints formatted JSON and exits 0.

- [ ] **Step 7: Commit**

```sh
git add spec/conformance
git commit -m "test: add sdk conformance fixtures"
```

### Task 3: Add Relay Chunk Specification

**Files:**
- Create: `spec/relay-chunks.md`
- Modify: `spec/relay-envelope.md`

- [ ] **Step 1: Create relay chunk spec**

Add `spec/relay-chunks.md`:

```markdown
# Relay Chunks

Relay chunks let a device without internet connectivity stream queued Honch data to a gateway, companion app, or hub. The gateway forwards data to Honch capture after durable receipt.

## Frame Format

| Offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | version | `1` |
| 1 | 1 | source_type | `1` for events |
| 2 | 1 | flags | bit 0 first chunk, bit 1 final chunk |
| 3 | 1 | reserved | `0` |
| 4 | 8 | sequence | uint64 big-endian |
| 12 | 4 | offset | uint32 big-endian |
| 16 | 2 | payload_length | uint16 big-endian |
| 18 | 2 | crc16 | CRC-16 over bytes 0-17 plus payload |
| 20 | n | payload | CBOR message bytes |

## Sender Rules

- Send chunks in ascending offset order.
- Keep the queued source message pending until the receiver acknowledges the complete message.
- On failed transfer, abort packetization and retry from offset 0 later.
- Use the smallest MTU-safe payload size selected by the port.

## Receiver Rules

- Reject unsupported versions.
- Reject nonzero reserved bytes.
- Reassemble by source device ID and sequence.
- Accept duplicate chunks when offset and payload bytes match already stored bytes.
- Acknowledge only after the complete message is durably stored or forwarded successfully.

## Initial Sources

- `1`: events

Additional source types require a spec update and conformance fixture.
```

- [ ] **Step 2: Replace the reserved relay SDK surface**

Modify `spec/relay-envelope.md` so the future SDK surface references chunks:

````markdown
## SDK Surface

Firmware SDKs expose relay chunks with:

```c
bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(honch_packetizer_t *packetizer, uint8_t *buffer, size_t buffer_size, size_t *out_size, bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);
```

The relay or companion app forwards the reassembled CBOR batch to capture and stamps relay metadata.
````

- [ ] **Step 3: Commit**

```sh
git add spec/relay-chunks.md spec/relay-envelope.md
git commit -m "docs: define relay chunk protocol"
```

### Milestone 2: Split C/POSIX Into Core Plus POSIX Port

Outcome: POSIX still works, but reusable behavior starts living under `core/`.

### Task 4: Create Core Directory And Move Reusable C Files

**Files:**
- Create: `core/CMakeLists.txt`
- Move: `c-core/honch/src/honch_cbor.c` to `core/src/honch_cbor.c`
- Move: `c-core/honch/src/honch_encoder.c` to `core/src/honch_encoder.c`
- Move: `c-core/honch/src/honch_json.c` to `core/src/honch_json.c`
- Create: `core/include/honch/core/status.h`
- Create: `core/include/honch/core/config.h`
- Create: `core/include/honch/core/honch.h`
- Modify: `c-core/CMakeLists.txt`

- [ ] **Step 1: Move reusable sources with Git**

Run:

```sh
mkdir -p core/src core/include/honch/core
git mv c-core/honch/src/honch_cbor.c core/src/honch_cbor.c
git mv c-core/honch/src/honch_encoder.c core/src/honch_encoder.c
git mv c-core/honch/src/honch_json.c core/src/honch_json.c
```

Expected: `git status --short` shows three renamed files.

- [ ] **Step 2: Add core status header**

Add `core/include/honch/core/status.h`:

```c
#ifndef HONCH_CORE_STATUS_H
#define HONCH_CORE_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_status {
    HONCH_OK = 0,
    HONCH_ERROR_INVALID_ARGUMENT = 1,
    HONCH_ERROR_OUT_OF_MEMORY = 2,
    HONCH_ERROR_IO = 3,
    HONCH_ERROR_TRANSPORT = 4,
    HONCH_ERROR_RATE_LIMITED = 5,
    HONCH_ERROR_SERVER = 6,
    HONCH_ERROR_REJECTED = 7,
    HONCH_ERROR_NOT_INITIALIZED = 8,
    HONCH_ERROR_ALREADY_INITIALIZED = 9,
    HONCH_ERROR_QUEUE_FULL = 10,
    HONCH_ERROR_TIMEOUT = 11,
    HONCH_ERROR_INTERNAL = 12
} honch_status_t;

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 3: Add core config header skeleton**

Add `core/include/honch/core/config.h`:

```c
#ifndef HONCH_CORE_CONFIG_H
#define HONCH_CORE_CONFIG_H

#include <stddef.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_platform_ops honch_platform_ops_t;
typedef struct honch_storage_ops honch_storage_ops_t;
typedef struct honch_transport_ops honch_transport_ops_t;

typedef struct honch_core_config {
    const char *api_key;
    const char *endpoint_url;
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *environment;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    size_t flush_event_threshold;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    int disable_gzip;
    size_t gzip_min_bytes;
    int disable_background_flush;
    int (*battery_callback)(void);
    int battery_low_threshold;
    const honch_platform_ops_t *platform;
    const honch_storage_ops_t *storage;
    const honch_transport_ops_t *transport;
} honch_core_config_t;

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 4: Add core public header skeleton**

Add `core/include/honch/core/honch.h`:

```c
#ifndef HONCH_CORE_HONCH_H
#define HONCH_CORE_HONCH_H

#include "honch/core/config.h"
#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_client honch_client_t;

honch_status_t honch_core_init(honch_client_t **client, const honch_core_config_t *config);
honch_status_t honch_core_track(honch_client_t *client, const char *event_name, const char *properties_json);
honch_status_t honch_core_identify(honch_client_t *client, const char *distinct_id, const char *traits_json);
honch_status_t honch_core_set_property(honch_client_t *client, const char *key, const char *value_json);
honch_status_t honch_core_session_start(honch_client_t *client, const char *session_name);
honch_status_t honch_core_session_end(honch_client_t *client);
honch_status_t honch_core_flush(honch_client_t *client);
honch_status_t honch_core_reset(honch_client_t *client);
honch_status_t honch_core_shutdown(honch_client_t *client);
const char *honch_core_get_device_id(honch_client_t *client);
honch_status_t honch_core_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size);
const char *honch_status_string(honch_status_t status);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 5: Add core CMake target**

Add `core/CMakeLists.txt`:

```cmake
add_library(honch_core
    src/honch_cbor.c
    src/honch_encoder.c
    src/honch_json.c
)

target_include_directories(honch_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(honch_core PUBLIC c_std_11)

target_compile_options(honch_core PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
```

- [ ] **Step 6: Update C/POSIX build to include core**

Modify `c-core/CMakeLists.txt`:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
```

Remove moved source files from `add_library(honch_c_core ...)` and link:

```cmake
target_link_libraries(honch_c_core
    PRIVATE
        honch_core
        CURL::libcurl
        Threads::Threads
)
```

- [ ] **Step 7: Build and test C/POSIX**

Run:

```sh
cmake -S c-core -B c-core/build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

Expected: build succeeds and `ctest` passes.

- [ ] **Step 8: Commit**

```sh
git add core c-core/CMakeLists.txt
git commit -m "refactor(core): introduce shared c core target"
```

### Task 5: Extract Platform, Storage, And Transport Interfaces

**Files:**
- Create: `core/include/honch/core/platform.h`
- Create: `core/include/honch/core/storage.h`
- Create: `core/include/honch/core/transport.h`
- Modify: `core/include/honch/core/config.h`

- [ ] **Step 1: Add platform interface**

Add `core/include/honch/core/platform.h`:

```c
#ifndef HONCH_CORE_PLATFORM_H
#define HONCH_CORE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_log_level {
    HONCH_LOG_DEBUG,
    HONCH_LOG_INFO,
    HONCH_LOG_WARN,
    HONCH_LOG_ERROR
} honch_log_level_t;

typedef struct honch_platform_ops {
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*uptime_ms)(void *ctx);
    honch_status_t (*random_bytes)(void *ctx, uint8_t *buffer, size_t buffer_size);
    honch_status_t (*lock)(void *ctx);
    honch_status_t (*unlock)(void *ctx);
    void (*log)(void *ctx, honch_log_level_t level, const char *message);
    void *ctx;
} honch_platform_ops_t;

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Add storage interface**

Add `core/include/honch/core/storage.h`:

```c
#ifndef HONCH_CORE_STORAGE_H
#define HONCH_CORE_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_storage_reader {
    void *ctx;
    honch_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buffer, size_t buffer_size);
    size_t total_size;
    uint64_t sequence;
} honch_storage_reader_t;

typedef struct honch_storage_ops {
    honch_status_t (*state_get)(void *ctx, const char *key, uint8_t *buffer, size_t *buffer_size);
    honch_status_t (*state_set)(void *ctx, const char *key, const uint8_t *data, size_t data_size);
    honch_status_t (*state_delete)(void *ctx, const char *key);
    honch_status_t (*queue_push)(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence);
    honch_status_t (*queue_peek)(void *ctx, honch_storage_reader_t *reader);
    honch_status_t (*queue_consume)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_dead_letter)(void *ctx, uint64_t sequence);
    honch_status_t (*queue_drop_oldest)(void *ctx);
    honch_status_t (*queue_clear)(void *ctx);
    honch_status_t (*queue_depth)(void *ctx, size_t *depth);
    void *ctx;
} honch_storage_ops_t;

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 3: Add transport interface**

Add `core/include/honch/core/transport.h`:

```c
#ifndef HONCH_CORE_TRANSPORT_H
#define HONCH_CORE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_transport_result {
    HONCH_TRANSPORT_ACCEPTED,
    HONCH_TRANSPORT_RETRY,
    HONCH_TRANSPORT_REJECTED,
    HONCH_TRANSPORT_AUTH_ERROR
} honch_transport_result_t;

typedef struct honch_transport_ops {
    honch_status_t (*post_batch)(
        void *ctx,
        const char *endpoint_url,
        const char *api_key,
        const uint8_t *body,
        size_t body_size,
        const char *content_encoding,
        honch_transport_result_t *result);
    void *ctx;
} honch_transport_ops_t;

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 4: Include interfaces from config**

Modify `core/include/honch/core/config.h` to include:

```c
#include "honch/core/platform.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"
```

Remove the forward declarations for `honch_platform_ops_t`, `honch_storage_ops_t`, and `honch_transport_ops_t`.

- [ ] **Step 5: Compile headers**

Run:

```sh
cmake --build c-core/build
```

Expected: build succeeds.

- [ ] **Step 6: Commit**

```sh
git add core/include/honch/core
git commit -m "feat(core): define platform storage and transport interfaces"
```

### Task 6: Move POSIX Implementations Behind Port Interface

**Files:**
- Create: `ports/posix/CMakeLists.txt`
- Create: `ports/posix/include/honch/posix/honch.h`
- Move: `c-core/honch/src/honch_platform.c` to `ports/posix/src/posix_platform.c`
- Move: `c-core/honch/src/honch_queue.c` to `ports/posix/src/posix_storage.c`
- Move: `c-core/honch/src/honch_state.c` to `ports/posix/src/posix_state.c`
- Move: `c-core/honch/src/honch_transport_curl.c` to `ports/posix/src/posix_transport_curl.c`
- Modify: `c-core/CMakeLists.txt`
- Modify: `c-core/honch/include/honch/honch.h`

- [ ] **Step 1: Move POSIX files**

Run:

```sh
mkdir -p ports/posix/src ports/posix/include/honch/posix
git mv c-core/honch/src/honch_platform.c ports/posix/src/posix_platform.c
git mv c-core/honch/src/honch_queue.c ports/posix/src/posix_storage.c
git mv c-core/honch/src/honch_state.c ports/posix/src/posix_state.c
git mv c-core/honch/src/honch_transport_curl.c ports/posix/src/posix_transport_curl.c
```

Expected: `git status --short` shows four renames.

- [ ] **Step 2: Add POSIX public wrapper header**

Add `ports/posix/include/honch/posix/honch.h`:

```c
#ifndef HONCH_POSIX_HONCH_H
#define HONCH_POSIX_HONCH_H

#include "honch/core/honch.h"

#endif
```

- [ ] **Step 3: Keep current C/POSIX include path compatible**

Modify `c-core/honch/include/honch/honch.h` so it includes the core public API and preserves aliases:

```c
#ifndef HONCH_HONCH_H
#define HONCH_HONCH_H

#include "honch/core/honch.h"

typedef honch_status_t honch_err_t;

#define HONCH_ERR_INVALID_ARG HONCH_ERROR_INVALID_ARGUMENT
#define HONCH_ERR_NOT_INITIALIZED HONCH_ERROR_NOT_INITIALIZED
#define HONCH_ERR_ALREADY_INITIALIZED HONCH_ERROR_ALREADY_INITIALIZED
#define HONCH_ERR_NO_MEM HONCH_ERROR_OUT_OF_MEMORY
#define HONCH_ERR_QUEUE_FULL HONCH_ERROR_QUEUE_FULL
#define HONCH_ERR_NVS HONCH_ERROR_IO
#define HONCH_ERR_TRANSPORT HONCH_ERROR_TRANSPORT
#define HONCH_ERR_TIMEOUT HONCH_ERROR_TIMEOUT
#define HONCH_ERR_INTERNAL HONCH_ERROR_INTERNAL

#endif
```

- [ ] **Step 4: Add POSIX CMake target**

Add `ports/posix/CMakeLists.txt`:

```cmake
add_library(honch_posix
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
)

target_include_directories(honch_posix
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../c-core/honch/include
)

target_link_libraries(honch_posix
    PUBLIC honch_core
    PRIVATE CURL::libcurl Threads::Threads
)

target_compile_definitions(honch_posix PRIVATE _POSIX_C_SOURCE=200809L)

target_compile_options(honch_posix PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
```

- [ ] **Step 5: Update C/POSIX build**

Modify `c-core/CMakeLists.txt`:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix ${CMAKE_CURRENT_BINARY_DIR}/ports/posix)
```

Make `honch_c_core` link to `honch_posix` while the compatibility layer still exists:

```cmake
target_link_libraries(honch_c_core
    PRIVATE
        honch_core
        honch_posix
)
```

- [ ] **Step 6: Build and test**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

Expected: all existing tests pass.

- [ ] **Step 7: Commit**

```sh
git add c-core ports/posix
git commit -m "refactor(posix): move platform code into posix port"
```

### Milestone 3: Make Core Own Client Behavior

Outcome: C/POSIX is a wrapper over `honch_core_*`.

### Task 7: Move Client State Machine Into Core

**Files:**
- Move: `c-core/honch/src/honch.c` to `core/src/honch_core.c`
- Create: `core/src/honch_internal.h`
- Modify: `core/CMakeLists.txt`
- Create: `ports/posix/src/posix_compat.c`
- Modify: `c-core/CMakeLists.txt`

- [ ] **Step 1: Move client implementation**

Run:

```sh
git mv c-core/honch/src/honch.c core/src/honch_core.c
```

Expected: one rename appears in `git status --short`.

- [ ] **Step 2: Rename public functions in core**

In `core/src/honch_core.c`, rename exported symbols:

```text
honch_init -> honch_core_init
honch_track -> honch_core_track
honch_identify -> honch_core_identify
honch_set_property -> honch_core_set_property
honch_session_start -> honch_core_session_start
honch_session_end -> honch_core_session_end
honch_flush -> honch_core_flush
honch_reset -> honch_core_reset
honch_shutdown -> honch_core_shutdown
honch_get_device_id -> honch_core_get_device_id
honch_copy_device_id -> honch_core_copy_device_id
```

- [ ] **Step 3: Add POSIX compatibility wrappers**

Add `ports/posix/src/posix_compat.c`:

```c
#include "honch/honch.h"
#include "honch/core/honch.h"

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config) {
    return honch_core_init(client, (const honch_core_config_t *)config);
}

honch_status_t honch_track(honch_client_t *client, const char *event_name, const char *properties_json) {
    return honch_core_track(client, event_name, properties_json);
}

honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const char *traits_json) {
    return honch_core_identify(client, distinct_id, traits_json);
}

honch_status_t honch_set_property(honch_client_t *client, const char *key, const char *value_json) {
    return honch_core_set_property(client, key, value_json);
}

honch_status_t honch_session_start(honch_client_t *client, const char *session_name) {
    return honch_core_session_start(client, session_name);
}

honch_status_t honch_session_end(honch_client_t *client) {
    return honch_core_session_end(client);
}

honch_status_t honch_flush(honch_client_t *client) {
    return honch_core_flush(client);
}

honch_status_t honch_reset(honch_client_t *client) {
    return honch_core_reset(client);
}

honch_status_t honch_shutdown(honch_client_t *client) {
    return honch_core_shutdown(client);
}

const char *honch_get_device_id(honch_client_t *client) {
    return honch_core_get_device_id(client);
}

honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size) {
    return honch_core_copy_device_id(client, buffer, buffer_size);
}
```

- [ ] **Step 4: Add moved implementation to core target**

Modify `core/CMakeLists.txt`:

```cmake
add_library(honch_core
    src/honch_core.c
    src/honch_cbor.c
    src/honch_encoder.c
    src/honch_json.c
)
```

- [ ] **Step 5: Add compatibility source to POSIX target**

Modify `ports/posix/CMakeLists.txt`:

```cmake
add_library(honch_posix
    src/posix_compat.c
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
)
```

- [ ] **Step 6: Build and test**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```sh
git add core ports/posix c-core
git commit -m "refactor(core): move client state machine into core"
```

### Task 8: Replace POSIX-Specific Calls In Core With Ops

**Files:**
- Modify: `core/src/honch_core.c`
- Modify: `core/src/honch_internal.h`
- Modify: `ports/posix/src/posix_platform.c`
- Modify: `ports/posix/src/posix_storage.c`
- Modify: `ports/posix/src/posix_transport_curl.c`
- Test: `core/test/test_storage_contract.c`

- [ ] **Step 1: Add failing storage contract test**

Add `core/test/test_storage_contract.c`:

```c
#include "honch/core/storage.h"

#include <assert.h>
#include <string.h>

int main(void) {
    honch_storage_reader_t reader = {0};
    assert(reader.total_size == 0u);
    assert(reader.sequence == 0u);
    assert(reader.read == 0);
    return 0;
}
```

Add it to `core/CMakeLists.txt` behind `HONCH_BUILD_TESTS`.

- [ ] **Step 2: Run test build**

Run:

```sh
cmake -S c-core -B c-core/build -DHONCH_BUILD_TESTS=ON
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

Expected: build succeeds after the test is wired.

- [ ] **Step 3: Move platform fields into core client**

In `core/src/honch_internal.h`, ensure `struct honch_client` stores:

```c
const honch_platform_ops_t *platform;
const honch_storage_ops_t *storage;
const honch_transport_ops_t *transport;
```

- [ ] **Step 4: Replace direct POSIX operations**

In `core/src/honch_core.c`, replace direct calls to POSIX file, pthread, and curl helpers with:

```c
client->platform->lock(client->platform->ctx);
client->platform->unlock(client->platform->ctx);
client->storage->state_get(client->storage->ctx, key, buffer, &buffer_size);
client->storage->state_set(client->storage->ctx, key, data, data_size);
client->storage->queue_push(client->storage->ctx, event, event_size, sequence);
client->transport->post_batch(client->transport->ctx, client->endpoint_url, client->api_key, body, body_size, encoding, &result);
```

- [ ] **Step 5: Implement POSIX ops**

In POSIX port files, expose:

```c
honch_status_t honch_posix_platform_ops_init(honch_platform_ops_t *ops, honch_posix_platform_t *ctx);
honch_status_t honch_posix_storage_ops_init(honch_storage_ops_t *ops, honch_posix_storage_t *ctx, const char *queue_directory);
honch_status_t honch_posix_transport_ops_init(honch_transport_ops_t *ops, honch_posix_transport_t *ctx);
```

Each init function fills the vtable and context pointer.

- [ ] **Step 6: Preserve POSIX public config**

In `ports/posix/src/posix_compat.c`, convert existing `honch_config_t` into `honch_core_config_t`, initialize POSIX ops, then call `honch_core_init`.

- [ ] **Step 7: Build and test**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```sh
git add core ports/posix c-core
git commit -m "refactor(core): route platform behavior through ops"
```

### Milestone 4: Packetizer And Relay

Outcome: core can export queued data without HTTP.

### Task 9: Add Packetizer Unit Tests

**Files:**
- Create: `core/include/honch/core/packetizer.h`
- Create: `core/src/honch_packetizer.c`
- Create: `core/test/test_packetizer.c`
- Modify: `core/CMakeLists.txt`

- [ ] **Step 1: Add packetizer public header**

Add `core/include/honch/core/packetizer.h`:

```c
#ifndef HONCH_CORE_PACKETIZER_H
#define HONCH_CORE_PACKETIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "honch/core/honch.h"
#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_data_source_mask {
    HONCH_DATA_SOURCE_EVENTS = 1u << 0,
    HONCH_DATA_SOURCE_LOGS = 1u << 1,
    HONCH_DATA_SOURCE_DIAGNOSTICS = 1u << 2,
    HONCH_DATA_SOURCE_ALL = HONCH_DATA_SOURCE_EVENTS | HONCH_DATA_SOURCE_LOGS | HONCH_DATA_SOURCE_DIAGNOSTICS
} honch_data_source_mask_t;

typedef struct honch_packetizer {
    honch_client_t *client;
    uint32_t source_mask;
    uint64_t sequence;
    uint32_t offset;
    size_t total_size;
    bool active;
} honch_packetizer_t;

bool honch_core_data_available(honch_client_t *client, uint32_t source_mask);
honch_status_t honch_packetizer_begin(honch_client_t *client, honch_packetizer_t *packetizer, uint32_t source_mask);
honch_status_t honch_packetizer_next(honch_packetizer_t *packetizer, uint8_t *buffer, size_t buffer_size, size_t *out_size, bool *message_complete);
honch_status_t honch_packetizer_confirm(honch_packetizer_t *packetizer);
honch_status_t honch_packetizer_abort(honch_packetizer_t *packetizer);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Add failing packetizer tests**

Add `core/test/test_packetizer.c` with tests for:

```c
static void test_tiny_buffer_rejected(void);
static void test_single_chunk_message_has_first_and_final_flags(void);
static void test_multi_chunk_message_offsets_increase(void);
static void test_abort_does_not_consume_storage(void);
static void test_confirm_consumes_storage(void);
```

Each test should use an in-memory fake storage implementation of `honch_storage_ops_t`.

- [ ] **Step 3: Run packetizer test before implementation**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure -R packetizer
```

Expected: test build or tests fail because packetizer functions are not implemented.

- [ ] **Step 4: Implement packetizer**

Add frame encoding in `core/src/honch_packetizer.c` using the `spec/relay-chunks.md` layout.

- [ ] **Step 5: Run packetizer tests**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure -R packetizer
```

Expected: packetizer tests pass.

- [ ] **Step 6: Commit**

```sh
git add core spec/relay-chunks.md
git commit -m "feat(core): add relay packetizer"
```

### Task 10: Make HTTP Flush Use Peek/Confirm

**Files:**
- Modify: `core/src/honch_core.c`
- Modify: `core/src/honch_queue_policy.c`
- Modify: `ports/posix/src/posix_storage.c`
- Modify: `ports/esp-idf/honch/src/esp_storage_nvs.c` after ESP port exists
- Test: `core/test/test_retry.c`

- [x] **Step 1: Add retry tests**

Create `core/test/test_retry.c` with cases:

```c
static void test_2xx_consumes_events(void);
static void test_401_dead_letters_events(void);
static void test_400_dead_letters_events(void);
static void test_429_preserves_events(void);
static void test_500_preserves_events(void);
static void test_network_error_preserves_events(void);
```

- [x] **Step 2: Run tests to expose current pop/requeue assumptions**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure -R retry
```

Expected: tests fail until core flush uses non-destructive peek.

- [x] **Step 3: Change core flush algorithm**

Implement:

```text
while queued data exists and batch has capacity:
  queue_peek next event
  validate event
  append event to batch
  remember sequence
post batch
if accepted:
  queue_consume each sequence
if auth/rejected:
  queue_dead_letter each sequence
if retry/network/server:
  leave sequences pending
```

- [x] **Step 4: Run retry tests**

Run:

```sh
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure -R retry
```

Expected: retry tests pass.

- [x] **Step 5: Run all C tests**

Run:

```sh
ctest --test-dir c-core/build --output-on-failure
```

Expected: all C/POSIX tests pass.

- [x] **Step 6: Commit**

```sh
git add core ports/posix
git commit -m "refactor(core): use peek confirm queue flushing"
```

### Milestone 5: ESP-IDF Port Over Core

Outcome: ESP-IDF public API remains stable, internals use canonical core.

### Task 11: Move ESP-IDF Files Under Ports Layout

**Files:**
- Move: `esp-idf/honch` to `ports/esp-idf/honch`
- Move: `esp-idf/example` to `ports/esp-idf/example`
- Move: `esp-idf/benchtest` to `ports/esp-idf/benchtest`
- Move: `esp-idf/tests` to `ports/esp-idf/tests`
- Modify: root `README.md`
- Modify: `.github/workflows/esp-idf.yml`

- [x] **Step 1: Move ESP-IDF tree**

Run:

```sh
mkdir -p ports/esp-idf
git mv esp-idf/honch ports/esp-idf/honch
git mv esp-idf/example ports/esp-idf/example
git mv esp-idf/benchtest ports/esp-idf/benchtest
git mv esp-idf/tests ports/esp-idf/tests
git mv esp-idf/README.md ports/esp-idf/README.md
```

Expected: `git status --short` shows renames.

- [x] **Step 2: Update paths in README and CI**

Replace `esp-idf/` references with `ports/esp-idf/` in:

```text
README.md
.github/workflows/esp-idf.yml
ports/esp-idf/README.md
ports/esp-idf/tests/test_cbor_migration.py
```

- [x] **Step 3: Run ESP-IDF static tests**

Run:

```sh
python3 ports/esp-idf/tests/test_cbor_migration.py
```

Expected: tests pass after path updates.

- [x] **Step 4: Commit**

```sh
git add README.md .github/workflows/esp-idf.yml ports/esp-idf
git commit -m "refactor(esp-idf): move sdk into ports tree"
```

### Task 12: Replace ESP-IDF Internals With Core Port Adapters

**Files:**
- Create: `ports/esp-idf/honch/src/esp_platform.c`
- Create: `ports/esp-idf/honch/src/esp_storage_nvs.c`
- Create: `ports/esp-idf/honch/src/esp_transport_http.c`
- Create: `ports/esp-idf/honch/src/esp_scheduler.c`
- Create: `ports/esp-idf/honch/src/esp_compat.c`
- Keep: `ports/esp-idf/honch/src/gpio.c` renamed to `esp_gpio_adapter.c`
- Modify: `ports/esp-idf/honch/CMakeLists.txt`
- Modify: `ports/esp-idf/honch/include/honch.h`

- [x] **Step 1: Add ESP-IDF platform ops**

`esp_platform.c` implements:

```c
uint64_t now_ms = esp_timer_get_time() / 1000;
uint64_t uptime_ms = esp_timer_get_time() / 1000;
random_bytes = esp_fill_random;
lock/unlock = FreeRTOS mutex wrappers;
log = ESP_LOGx wrappers;
```

- [x] **Step 2: Add ESP-IDF storage ops**

`esp_storage_nvs.c` implements `honch_storage_ops_t` using:

```text
state namespace: honch_state
queue namespace: honch_q
head/tail counters for sequence
blob keys by sequence number
peek reads tail without deleting
consume erases confirmed sequence
dead_letter erases or writes into honch_dead namespace when enabled
drop_oldest erases tail
```

- [x] **Step 3: Add ESP-IDF transport ops**

`esp_transport_http.c` wraps existing `esp_http_client` behavior and returns `honch_transport_result_t`.

- [x] **Step 4: Add ESP-IDF compatibility layer**

`esp_compat.c` keeps the current ESP-IDF public API:

```c
honch_err_t honch_init(const honch_config_t *config);
honch_err_t honch_shutdown(void);
honch_err_t honch_track(const char *event, const char *properties_json);
honch_err_t honch_identify(const char *distinct_id, const char *properties_json);
honch_err_t honch_set_property(const char *key, const char *value_json);
honch_err_t honch_session_start(const char *session_name);
honch_err_t honch_session_end(void);
honch_err_t honch_flush(void);
honch_err_t honch_reset(void);
const char *honch_get_device_id(void);
```

Each function delegates to the global `honch_client_t *` core client.

- [x] **Step 5: Keep GPIO as adapter**

Rename `gpio.c` to `esp_gpio_adapter.c`. It should call `honch_track` from a worker task, not from ISR context. Its behavior remains:

```text
max 8 pins
50 ms debounce
rising/falling/both edge modes
properties include {"pin": <number>}
```

- [ ] **Step 6: Build ESP-IDF example**

Run from an ESP-IDF shell:

```sh
cd ports/esp-idf/example
idf.py build
```

Expected: build succeeds.

- [ ] **Step 7: Run ESP-IDF static tests**

Run:

```sh
python3 ports/esp-idf/tests/test_cbor_migration.py
```

Expected: tests pass.

- [ ] **Step 8: Commit**

```sh
git add ports/esp-idf core
git commit -m "refactor(esp-idf): port sdk onto canonical core"
```

### Milestone 6: React Native Relay Path

Outcome: BLE-only products have a first-class path.

### Task 13: Add React Native Relay Package Skeleton

**Files:**
- Create: `ports/react-native-relay/README.md`
- Create: `ports/react-native-relay/package.json`
- Create: `ports/react-native-relay/src/index.ts`
- Create: `ports/react-native-relay/src/frame.ts`
- Create: `ports/react-native-relay/src/relayQueue.ts`
- Create: `ports/react-native-relay/src/uploader.ts`
- Create: `ports/react-native-relay/test/frame.test.ts`

- [ ] **Step 1: Add package metadata**

Add `ports/react-native-relay/package.json`:

```json
{
  "name": "@honch/react-native-relay",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "main": "src/index.ts",
  "scripts": {
    "test": "vitest run",
    "typecheck": "tsc --noEmit"
  },
  "dependencies": {},
  "devDependencies": {
    "typescript": "^5.0.0",
    "vitest": "^1.0.0"
  }
}
```

- [ ] **Step 2: Add frame decoder**

Add `ports/react-native-relay/src/frame.ts`:

```ts
export type RelayFrame = {
  version: number;
  sourceType: number;
  first: boolean;
  final: boolean;
  sequence: bigint;
  offset: number;
  payload: Uint8Array;
};

export function decodeRelayFrame(bytes: Uint8Array): RelayFrame {
  if (bytes.length < 20) {
    throw new Error("relay frame too short");
  }
  const version = bytes[0];
  if (version !== 1) {
    throw new Error("unsupported relay frame version");
  }
  if (bytes[3] !== 0) {
    throw new Error("relay frame reserved byte must be zero");
  }
  const payloadLength = (bytes[16] << 8) | bytes[17];
  if (bytes.length !== 20 + payloadLength) {
    throw new Error("relay frame payload length mismatch");
  }
  let sequence = 0n;
  for (let i = 4; i < 12; i += 1) {
    sequence = (sequence << 8n) | BigInt(bytes[i]);
  }
  const offset = (bytes[12] << 24) | (bytes[13] << 16) | (bytes[14] << 8) | bytes[15];
  return {
    version,
    sourceType: bytes[1],
    first: (bytes[2] & 1) !== 0,
    final: (bytes[2] & 2) !== 0,
    sequence,
    offset,
    payload: bytes.slice(20)
  };
}
```

- [ ] **Step 3: Add frame tests**

Add `ports/react-native-relay/test/frame.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { decodeRelayFrame } from "../src/frame";

describe("decodeRelayFrame", () => {
  it("decodes a valid empty payload final frame", () => {
    const frame = new Uint8Array(20);
    frame[0] = 1;
    frame[1] = 1;
    frame[2] = 3;
    frame[11] = 7;
    const decoded = decodeRelayFrame(frame);
    expect(decoded.version).toBe(1);
    expect(decoded.sourceType).toBe(1);
    expect(decoded.first).toBe(true);
    expect(decoded.final).toBe(true);
    expect(decoded.sequence).toBe(7n);
    expect(decoded.offset).toBe(0);
    expect(decoded.payload.length).toBe(0);
  });

  it("rejects unsupported versions", () => {
    const frame = new Uint8Array(20);
    frame[0] = 2;
    expect(() => decodeRelayFrame(frame)).toThrow("unsupported relay frame version");
  });
});
```

- [ ] **Step 4: Commit**

```sh
git add ports/react-native-relay
git commit -m "feat(relay): add react native relay package skeleton"
```

### Task 14: Define Relay Upload Behavior

**Files:**
- Modify: `ports/react-native-relay/src/relayQueue.ts`
- Modify: `ports/react-native-relay/src/uploader.ts`
- Modify: `ports/react-native-relay/src/index.ts`
- Test: `ports/react-native-relay/test/relayQueue.test.ts`

- [ ] **Step 1: Add relay queue interface**

Add `ports/react-native-relay/src/relayQueue.ts`:

```ts
export type StoredRelayMessage = {
  deviceId: string;
  sourceType: number;
  sequence: string;
  body: Uint8Array;
};

export interface RelayQueue {
  putChunk(deviceId: string, frameBytes: Uint8Array): Promise<{ complete: boolean; message?: StoredRelayMessage }>;
  markUploaded(deviceId: string, sequence: string): Promise<void>;
  pending(): Promise<StoredRelayMessage[]>;
}
```

- [ ] **Step 2: Add uploader interface**

Add `ports/react-native-relay/src/uploader.ts`:

```ts
import type { StoredRelayMessage } from "./relayQueue";

export type RelayUploaderConfig = {
  endpointUrl: string;
  apiKey: string;
  relayId: string;
  relaySdkPlatform: string;
  relaySdkVersion: string;
};

export async function uploadRelayMessage(config: RelayUploaderConfig, message: StoredRelayMessage): Promise<void> {
  const response = await fetch(`${config.endpointUrl.replace(/\/$/, "")}/batch`, {
    method: "POST",
    headers: {
      "Content-Type": "application/cbor",
      "X-Honch-Relay-Id": config.relayId,
      "X-Honch-Relay-SDK-Platform": config.relaySdkPlatform,
      "X-Honch-Relay-SDK-Version": config.relaySdkVersion,
      "Authorization": `Bearer ${config.apiKey}`
    },
    body: message.body
  });
  if (!response.ok) {
    throw new Error(`relay upload failed: ${response.status}`);
  }
}
```

- [ ] **Step 3: Export relay APIs**

Modify `ports/react-native-relay/src/index.ts`:

```ts
export { decodeRelayFrame } from "./frame";
export type { RelayFrame } from "./frame";
export type { RelayQueue, StoredRelayMessage } from "./relayQueue";
export { uploadRelayMessage } from "./uploader";
export type { RelayUploaderConfig } from "./uploader";
```

- [ ] **Step 4: Add queue behavior tests**

Add `ports/react-native-relay/test/relayQueue.test.ts` with tests for:

```text
complete message only ACKs after final chunk
duplicate chunk is idempotent
mismatched duplicate chunk is rejected
pending returns complete unuploaded messages
markUploaded removes uploaded message
```

- [ ] **Step 5: Run relay tests**

Run:

```sh
cd ports/react-native-relay
npm run typecheck
npm test
```

Expected: typecheck and tests pass.

- [ ] **Step 6: Commit**

```sh
git add ports/react-native-relay
git commit -m "feat(relay): define mobile relay upload path"
```

### Milestone 7: MicroPython Conformance

Outcome: MicroPython remains usable while core becomes canonical.

### Task 15: Move MicroPython Under Ports And Add Conformance Runner

**Files:**
- Move: `micropython/` to `ports/micropython/`
- Create: `ports/micropython/tests/test_conformance.py`
- Modify: `.github/workflows/micropython.yml`
- Modify: `README.md`

- [ ] **Step 1: Move MicroPython**

Run:

```sh
git mv micropython ports/micropython
```

Expected: `git status --short` shows the directory rename.

- [ ] **Step 2: Add conformance runner**

Add `ports/micropython/tests/test_conformance.py`:

```python
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def load_fixture(path):
    return json.loads((ROOT / path).read_text())


def test_basic_track_fixture_loads():
    fixture = load_fixture("spec/conformance/events/basic-track.json")
    assert fixture["name"] == "basic track"
    assert fixture["expect"]["distinct_id"] == "dev_fixture"


def test_http_response_policy_fixture_loads():
    fixture = load_fixture("spec/conformance/http/response-policy.json")
    assert any(case["status"] == 429 and case["queue"] == "preserve" for case in fixture["cases"])
```

- [ ] **Step 3: Update workflow paths**

Replace `micropython/` with `ports/micropython/` in `.github/workflows/micropython.yml`.

- [ ] **Step 4: Run MicroPython tests**

Run:

```sh
cd ports/micropython
python3 -m unittest discover tests
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```sh
git add README.md .github/workflows/micropython.yml ports/micropython
git commit -m "refactor(micropython): move sdk into ports tree"
```

## Verification Gates

Run these gates before merging the full refactor branch:

```sh
git diff --check
```

```sh
cmake -S c-core -B c-core/build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build c-core/build
ctest --test-dir c-core/build --output-on-failure
```

```sh
python3 ports/esp-idf/tests/test_cbor_migration.py
```

```sh
cd ports/micropython
python3 -m unittest discover tests
```

```sh
cd ports/react-native-relay
npm run typecheck
npm test
```

From an ESP-IDF environment:

```sh
cd ports/esp-idf/example
idf.py build
```

## Expected End State

- One canonical C core owns SDK behavior.
- POSIX is a port over the core, not the core itself.
- ESP-IDF is a port over the core while preserving public API compatibility.
- MicroPython is either a wrapper or a conformance-compatible implementation with shared fixtures.
- React Native relay has a clear chunk protocol and mobile upload path.
- Queue policy is non-destructive peek/confirm.
- Direct HTTP and BLE relay both drain the same canonical queued data.
- Optional hardware features such as GPIO remain adapters, not core behavior.

## Rollout Notes

- Keep the old ESP-IDF public API during the refactor.
- Keep the old C/POSIX public API during the refactor.
- Do not publish breaking package changes until compatibility examples pass.
- Treat relay as a first-class transport path before adding new analytics modules.
- Keep feature flags simple until real size pressure appears.

## Self-Review

- Spec coverage: The plan covers canonical core ownership, platform ports, storage, transport, packetizer, BLE/mobile relay, ESP-IDF migration, POSIX migration, MicroPython conformance, and verification gates.
- Placeholder scan: The plan does not contain unresolved deferred-work markers.
- Type consistency: Core names use `honch_core_*`; compatibility wrappers preserve existing `honch_*` APIs. Storage uses `queue_peek`, `queue_consume`, and `queue_dead_letter` consistently. Packetizer names match between the relay spec and core API.
