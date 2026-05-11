# Wire Format

All Honch SDKs send events to the capture endpoint using the CBOR ingest contract.

## Endpoint

```
POST <host>/batch
Content-Type: application/cbor
```

The body is an uncompressed CBOR map. SDKs may use gzip only when explicitly configured for a platform that benefits from it.

## Batch Envelope

```text
{
  "token": "<api_key>",
  "batch": [
    {
      "event": "<event_name>",
      "distinct_id": "<distinct_id>",
      "timestamp": 1770000000000,
      "properties": { ... }
    }
  ]
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `token` | string | Yes | Project API key |
| `batch` | array | Yes | Array of event objects (1-50 per request) |

### Event Object

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `event` | string | Yes | Event name (e.g. `"button_pressed"`, `"$device_boot"`) |
| `distinct_id` | string | Yes | User or device identifier |
| `timestamp` | integer | Yes | Epoch milliseconds, set when `track()` is called |
| `properties` | map | Yes | All event properties (auto-stamped + user-supplied) |

## CBOR Profile

- Use definite-length maps and arrays.
- Use text keys, not compact integer keys.
- Encode timestamps as signed epoch milliseconds.
- Encode properties using JSON-compatible values: text, integer, float, boolean, null, arrays, and maps.
- Do not use CBOR tags or byte-string property values for v1.

## Response Codes

| Code | Meaning | SDK Behavior |
|------|---------|-------------|
| 2xx | Success | Remove events from queue |
| 401 | Bad API key | Drop batch, log error, do not retry |
| 5xx | Server error | Retain events, retry with exponential backoff |
| Network error | Connection failed | Retain events, retry with exponential backoff |

## Retry Strategy

- Initial backoff: 1 second
- Max backoff: 5 minutes
- Jitter: +/-25% on each backoff interval
- On success: reset backoff to initial value

## Batch Size Limits

- Maximum 50 events per batch.
- SDKs should drain the queue in batches, sending multiple requests if needed.
