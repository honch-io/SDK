# Error Tracking Platform Handoff

This note is for the platform/frontend implementation of Honch SDK `$error`
events. Do not depend on the old platform experiment branch. Treat this as the
contract the UI should implement against.

## Event Shape

The SDK sends crashes as normal event rows where:

- `event` is `"$error"`.
- Standard top-level event context still applies: device ID, model, firmware
  version, SDK platform, environment, session, timestamps.
- Crash-specific values live in `properties`.

Expected `$error.properties` keys:

```json
{
  "source": "panic",
  "severity": "fatal",
  "reset_reason": "panic",
  "crash_summary_version": 1,
  "firmware_build_id": "059e855ed",
  "exception_cause": "LoadProhibited",
  "fault_pc": "0x400de120",
  "backtrace": "0x400de120,0x400dbeef,0x400d1234",
  "task_name": "main"
}
```

All crash fields except `source`, `severity`, and `reset_reason` are optional.
Some devices will only send reset metadata. Some will send raw addresses. Some
may later have symbolicated fields added by the backend.

## Optional Backend Decoration

If the backend has symbols for the matching firmware build, it can decorate the
same properties object with:

```json
{
  "symbolicated_fault": "sensor_read_temperature at sensor.c:184",
  "symbolicated_backtrace": [
    "sensor_read_temperature at sensor.c:184",
    "read_sensor_loop at main.c:92",
    "app_main at app_main.c:241"
  ]
}
```

Recommended backend behavior:

- Only decorate `event === "$error"`.
- Require both `firmware_build_id` and a valid hex `fault_pc`.
- Accept `backtrace` as either a comma/whitespace-separated string or an array.
- Cap normalized frames at 32.
- If symbols are unavailable or `addr2line` fails, return the original event
  unchanged.
- Strip source directories before returning symbolicated strings. Return
  `sensor.c:184`, not customer-local absolute paths.
- Do not add new ClickHouse columns for this first pass. Decoration can happen
  in the event mapper/API response.

## Frontend Rendering

Render a crash report section only when `event.event === "$error"`.

Summary fields:

- `source`
- `severity`
- `reset_reason`
- `firmware_build_id`
- `fault_pc`
- `task_name` if present

Fault line:

- Prefer `symbolicated_fault`.
- Fall back to raw `fault_pc`.
- If neither exists, show a low-key empty state such as "Fatal reset captured
  without address context".

Backtrace:

- Prefer `symbolicated_backtrace`.
- Fall back to raw `backtrace`.
- Accept raw `backtrace` as either an array or a string split on commas and
  whitespace.
- Filter non-string and blank values.
- Cap display to 32 frames.
- Preserve duplicate frames; use index plus duplicate count for stable React
  keys.

Suggested TypeScript shape:

```ts
export interface CrashEventProperties {
  source?: string;
  severity?: string;
  reset_reason?: string;
  crash_summary_version?: number;
  firmware_build_id?: string;
  exception_cause?: string;
  fault_pc?: string;
  backtrace?: unknown;
  task_name?: string;
  symbolicated_fault?: string;
  symbolicated_backtrace?: unknown;
}
```

Suggested helpers:

```ts
function stringProp(value: unknown): string | null {
  return typeof value === "string" && value.trim() ? value : null;
}

function listProp(value: unknown): string[] {
  if (Array.isArray(value)) {
    return value
      .map((item) => stringProp(item))
      .filter((item): item is string => Boolean(item))
      .slice(0, 32);
  }

  if (typeof value === "string") {
    return value
      .split(/[\s,]+/)
      .map((item) => item.trim())
      .filter(Boolean)
      .slice(0, 32);
  }

  return [];
}
```

## UX Notes

- Make this look like diagnostics, not a marketing card.
- Keep it compact in event details; this is one section above or near raw
  properties.
- Use symbolicated values when present, but do not hide raw metadata entirely.
- Explain missing symbolication as "Raw crash addresses; upload matching symbols
  to resolve source lines" or similar.
- Do not call this full crash forensics. The SDK sends lightweight crash
  summary metadata, not coredumps or RAM/register snapshots.

## Acceptance Tests

Frontend:

- Does not render the crash section for non-`$error` events.
- Renders `source`, `severity`, `reset_reason`, `firmware_build_id`, and
  `fault_pc` for raw `$error` events.
- Prefers `symbolicated_fault` over `fault_pc`.
- Prefers `symbolicated_backtrace` over `backtrace`.
- Parses raw `backtrace` from both string and array forms.
- Handles missing address context without crashing or showing empty chrome.

Backend, if symbolication decoration is implemented:

- Decorates only `$error` events.
- Returns the original properties when symbols are missing.
- Rejects unsafe project/build path segments for symbol file lookup.
- Caps uploaded symbol file size and stores symbol files outside web-served
  paths.
- Strips source directories from `addr2line` output.
- Leaves event ingest and ClickHouse schema unchanged.
