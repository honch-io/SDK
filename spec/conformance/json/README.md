# JSON Ingestion Conformance Fixtures

These fixtures define the behavior of the **JSON ingestion path** (`POST /capture`
with `Content-Type: application/json`). They are the contract for third parties who
build their own client against the HTTP API instead of using an official SDK.

Each fixture is a self-contained request plus its expected result. The valid
fixtures reuse the **same expanded events** as the equivalent `wire-v2/` binary
fixtures — proving that the JSON path and the binary path expand to identical
canonical events. (`single-required-context` and `custom-properties` mirror the
wire-v2 fixtures of the same name byte-for-result.)

## How these stay honest

These are **hand-authored** (not byte-generated like `wire-v2/`, which needs a tool
to produce valid binary frames — JSON needs no such generator). Hand-authoring is
correct here precisely *because* the expected outputs are independent of the code:
the capture service runs every fixture in this directory through the real ingestion
path and asserts the documented `expected_*` values
(`json_conformance_fixtures_match_expansion` in `platform`), so a fixture can't drift
from behavior. (Auto-generating the expected outputs would be worse — it would bless
regressions instead of catching them.)

The `platform` repo keeps a byte-identical mirror of these files for that test; a
second test (`json_conformance_copy_matches_canonical_sdk_fixtures`) fails if the
mirror diverges from this canonical copy. To re-sync after editing here:

```bash
cp sdks/spec/conformance/json/*.json platform/capture/tests/fixtures/json/
```

## Fixture Schema

```jsonc
{
  "name": "single-required-context",
  "description": "...",
  "wire_format": "honch-json-v1",
  "request": {
    "method": "POST",
    "path": "/capture",
    "headers": {
      "Content-Type": "application/json",
      "X-Honch-Project-Key": "<project_api_key>"
    },
    "body": { "context": { ... }, "events": [ ... ] }
  },

  // HTTP status returned by POST /capture for this body.
  "expected_ingest_status": 200,

  // The canonical events capture stores and that POST /capture/validate returns
  // under "expanded_events". Only the deterministic core is asserted (event,
  // distinct_id, properties, timestamp); capture additionally stamps a random
  // uuid, received_at, and ip/geo. Absent for whole-request rejections.
  "expected_expanded_events": [
    { "distinct_id": "...", "event": "...", "properties": { ... }, "timestamp": 1700000000000 }
  ],

  // For partial fixtures: counts in the 200 response body. Omitted means
  // accepted = expected_expanded_events.length and rejected = 0.
  "expected_accepted": 2,
  "expected_rejected": 2,

  // The FieldError `code`s expected in the response `errors` array (for rejected
  // events on a 200, or for the failures on a 4xx).
  "expected_error_codes": ["invalid_event", "invalid_property_value"]
}
```

## Response model

`POST /capture` returns `200` with `{ "status": "ok", "accepted": N, "rejected": M, "errors": [...] }`.
Capture uses **partial acceptance**: a bad event is dropped and reported in
`errors`, but the rest of the batch is still stored. The rule:

> `2xx` ⇒ at least one event stored. `4xx` ⇒ nothing stored.

A whole-request `4xx` happens only for bad/missing shared `context`, an empty or
oversized batch, or when *every* event is individually invalid. `POST
/capture/validate` mirrors this dry-run, returning
`{ ok, accepted, rejected, expanded_events, errors }` and storing nothing.

## How to use them

1. Encode `request.body` as JSON and POST it with the headers shown. Assert the
   HTTP status equals `expected_ingest_status`, and (when present) that the body's
   `accepted`/`rejected` match `expected_accepted`/`expected_rejected`.
2. To check expansion, POST the same body to `POST /capture/validate` and compare
   `expanded_events` against `expected_expanded_events` (ignoring `uuid`,
   `received_at`, `ip`, `geo_country`, `geo_city`).
3. Assert the response `errors[].code` set contains every entry in
   `expected_error_codes`.

`timestamp` values are epoch **milliseconds**. `1700000000000` = 2023-11-14T22:13:20Z.
