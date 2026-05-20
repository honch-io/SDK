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
