# Wire Format

All Honch SDKs send events to the capture endpoint using this format.

## Endpoint

```
POST <host>/batch
Content-Type: application/json
Content-Encoding: gzip
```

The body is always gzip-compressed JSON.

## Batch Envelope

```json
{
  "token": "<api_key>",
  "batch": [
    {
      "event": "<event_name>",
      "distinct_id": "<distinct_id>",
      "timestamp": "<ISO-8601 UTC with ms>",
      "properties": { ... }
    }
  ]
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `token` | string | Yes | Project API key |
| `batch` | array | Yes | Array of event objects (1–50 per request) |

### Event Object

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `event` | string | Yes | Event name (e.g. `"button_pressed"`, `"$device_boot"`) |
| `distinct_id` | string | Yes | User or device identifier |
| `timestamp` | string | Yes | ISO-8601 UTC, millisecond precision (e.g. `"2026-05-01T10:15:32.000Z"`) |
| `properties` | object | Yes | All event properties (auto-stamped + user-supplied) |

## Timestamps

- Set at event creation time (`track()` call), not at flush time.
- Format: `YYYY-MM-DDTHH:MM:SS.mmmZ`
- Always UTC.

## Compression

- Bodies are always gzip-encoded.
- The `Content-Encoding: gzip` header must be set.
- SDKs should use platform-native gzip (e.g. `miniz` on ESP-IDF, `zlib` on POSIX, `NSData` compression on iOS).

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
