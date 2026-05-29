# Wire Format

Status: Archived overview. The active compact binary upload contract is
[`../wire-format-v2.md`](../wire-format-v2.md).

Honch SDKs upload events with compact chunk frames. This document is retained
for historical context; new implementation work should use the active wire-v2
spec.

## Endpoint

```text
POST /capture
Content-Type: application/vnd.honch.chunk
X-Honch-Project-Key: <project_api_key>
X-Honch-Stream-Id: <stream_id>         # required for multi-chunk HTTP uploads
X-Honch-Relay-Id: <gateway_id>         # optional
X-Honch-Relay-SDK-Platform: <platform> # optional
X-Honch-Relay-SDK-Version: <version>   # optional
```

`/e` and `/chunks` are capture aliases for the same ingest path.

The request body contains exactly one chunk frame. `Content-Encoding` is not
allowed for chunk frames. Compression can be added later as a frame-level
feature, but the current upload path is one deterministic binary format.

The project key is sent as a transport credential and is not encoded in the
device message body. This keeps the compact device message reusable across
direct and relay transports without repeating a long token inside every upload.

## Upload Model

Every upload uses the chunk frame, including direct Wi-Fi HTTP. A direct HTTP
upload that fits in one request sends a single final frame. Transports with
smaller MTUs split the same compact message into multiple frames.

Capture must accept:

- a single complete frame;
- an init frame followed by continuation frames for the same message;
- duplicate frames whose payload bytes match already stored bytes.

Capture must reject:

- unsupported protocol versions;
- unsupported source types;
- nonzero reserved bits;
- offset mismatches;
- message ID collisions with different bytes;
- CRC failures;
- messages that exceed configured limits;
- frames for expired or unknown partial messages.

## Full Encoding

The authoritative binary layout is [wire-format-v2.md](../wire-format-v2.md).
That file describes the current compact
chunk frame, compact message, string table, context, event, value, response,
capture, SDK, relay, and fixture requirements.

No active SDK or capture path should expose alternate upload contracts.
