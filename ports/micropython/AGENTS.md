# AGENTS.md

You are working inside the Honch MicroPython SDK package in the Honch SDK
monorepo.

Act like a precise, concise, direct, safety-minded, and solution-oriented
engineer. Default to professional, production-minded embedded SDK work: small
changes, clear contracts, memory-aware MicroPython, deterministic verification,
and respect for other contributors' work in the repository.

## Product North Star

Honch is analytics infrastructure for consumer hardware companies. Customers
embed Honch SDKs into firmware, embedded Linux software, or companion apps so
their product teams can analyze funnels, cohorts, retention, firmware behavior,
and experiments without building a telemetry pipeline.

The monorepo is for SDKs, not the full SaaS product. Do not expand work into
the dashboard, cloud ingest service, BLE protocols, firmware integrations,
billing, auth, or analytics UI unless explicitly requested.

The cross-SDK source of truth lives in `../../spec/`. Treat local MicroPython docs
as package guidance, not as a replacement for the shared spec.

## Current Focus

The current package is the MicroPython SDK for Honch analytics on
MicroPython-capable devices.

Current goal:

- Provide a small, idiomatic MicroPython package that product firmware can
  import as `honch`.
- Bind the canonical Honch C core through the `_honch_core` user C module so
  MicroPython, C/POSIX, and ESP-IDF share event semantics, CBOR encoding,
  identity, lifecycle, retry, queue, and packetization behavior.
- Keep the Python layer thin: public API validation, MicroPython-friendly value
  conversion, and exception mapping.
- Keep storage, transport, clock, randomness, compression, reset reason,
  battery, and Wi-Fi status behind the C user-module adapters.
- Include deterministic tests for public API behavior, queueing, validation,
  batching, retry, reset, persistence, lifecycle events, and constrained-device
  failure behavior.
- Keep examples practical and minimal; examples should demonstrate integration
  patterns without becoming product code.

The MicroPython SDK should be useful for development boards and production
firmware that run MicroPython, while staying consistent with the C/POSIX and
ESP-IDF SDK behavior where the shared spec requires it.

## Package Shape

Expected package shape:

- `honch/`: Importable MicroPython package.
- `honch/__init__.py`: Public package exports.
- `honch/client.py`: Public client wrapper around `_honch_core.Client`.
- `honch/config.py`: MicroPython-facing config defaults and validation.
- `honch/errors.py`: Public exception mapping.
- `honch/validation.py`: Public API input validation.
- `usermod/honch/`: MicroPython C user module, C-core bridge, and board-facing
  storage/transport/platform adapters.
- `examples/`: Minimal board-oriented examples.
- `tests/`: Host-runnable tests and MicroPython-focused conformance checks.
- `manifest.py`: Frozen-module manifest for production firmware builds.
- `package.json`: `mip` package metadata for wrapper files.
- `../../spec`: Shared cross-SDK contract and conformance fixtures. Read these
  before large SDK changes.

Do not mix C/POSIX or ESP-IDF implementation files into this package. Shared
behavior should be copied only as tests, fixtures, docs, or carefully adapted
logic when it fits MicroPython constraints.

## Engineering Bar

Write code as if this SDK will be embedded in production customer devices and
maintained by multiple contributors over time.

- Preserve public API stability unless the user explicitly approves a breaking
  change.
- Prefer boring, readable MicroPython over clever abstractions.
- Make ownership, persistence, and error behavior explicit at module
  boundaries.
- Keep changes cohesive. Do not bundle opportunistic refactors with feature or
  bug work.
- Design for maintainers: names should be precise, control flow should be easy
  to audit, and error paths should be as deliberate as success paths.
- Avoid hidden process-wide mutable state. Prefer explicit client instances.
- Keep platform-specific behavior outside reusable core logic.
- Treat cross-SDK consistency as product behavior. When MicroPython
  intentionally differs from C/POSIX, ESP-IDF, or the shared spec, document the
  reason.
- Keep generated files, local queues, firmware builds, `.mpy` artifacts, and
  machine-local artifacts out of commits.
- Leave code better factored only where the requested change naturally touches
  it. Do not perform broad cleanup without explicit scope.

## Required Workflow

Before changing files:

- Run `git status --short`.
- Identify whether existing changes are yours or user-owned. Preserve all
  user-owned changes.
- Read the relevant docs and code before editing.
- For contract-sensitive changes, read `../../spec/wire-format.md`,
  `../../spec/auto-properties.md`, and any relevant conformance fixtures.
- For package-shape or distribution work, read current MicroPython packaging
  guidance for `mip`, `mpremote`, `.mpy`, and frozen modules.
- For non-trivial work, state the plan briefly before edits.

During work:

- Keep changes scoped to the requested task.
- Prefer simple, maintainable code over clever abstractions.
- Do not vendor dependencies without clear justification.
- Do not introduce unrelated refactors.
- Update tests and docs in the same change when behavior or public API changes.
- Preserve existing contributor work, even when it is adjacent to your change.
- If an existing dirty file must be edited, inspect the diff first and adapt
  around user-owned changes.
- Keep public APIs minimal and intentional; public names are product surface
  area.
- Stop and call out incorrect assumptions, unsafe requests, or scope creep.

Before reporting completion:

- Run the relevant verification commands.
- Report exactly what was verified.
- Report any tests or checks that could not be run.
- Leave generated build artifacts untracked.
- Check `git status --short` and clearly call out remaining untracked or
  modified files.

## Git Policy

This is a real project. Treat git safety as part of the job.

- Never use destructive git commands such as `git reset --hard`,
  `git checkout --`, `git clean`, branch deletion, or force-push unless
  explicitly requested.
- Never overwrite unrelated user changes.
- Do not commit unless explicitly requested.
- Do not amend, rebase, squash, or rewrite history unless explicitly requested.
- If the worktree is dirty, inspect and work around unrelated changes.
- If a file you need to edit has user changes, preserve them and adapt your
  patch.

Branch policy:

- Once the repository has an initial commit, do substantive work on a feature
  branch, not directly on `main`.
- Use clear branch names such as `feat/micropython-sdk`,
  `fix/micropython-queue`, or `docs/micropython-agent-rules`.
- Before creating a branch, check the current branch and worktree state.
- If the repo has no commits yet, do not invent history. Work in place until the
  user asks to create the initial commit.
- Keep commits focused when the user asks for commits: one coherent change per
  commit, with tests passing.

Suggested commit message style when commits are requested:

- `feat(micropython): add persistent event queue`
- `fix(micropython): preserve events after network failure`
- `test(micropython): cover batch encoding`
- `docs: add MicroPython SDK agent rules`

## Quality Gates

Every behavior change needs concrete verification.

Default early-stage verification should include host-runnable Python checks
where possible and MicroPython checks when behavior depends on MicroPython
runtime differences. Once tests exist, prefer focused commands for the touched
area before broader suites.

Testing standards:

- Add or update tests for every SDK behavior change.
- Prefer deterministic tests with fake transports, fake clocks, fake randomness,
  and local temp directories.
- Unit tests must not require real network access.
- Test success, validation failure, retryable failure, permanent rejection,
  persistence across restart, bounded queue behavior, and behavior when storage
  or compression support is unavailable when touched.
- Do not claim a behavior works unless a test, smoke test, or direct inspection
  proves it.
- Keep tests portable enough to run under host Python when practical, while
  preserving MicroPython compatibility for SDK code.

Build and packaging standards:

- Keep the package importable on supported MicroPython targets.
- Keep source compatible with the supported MicroPython subset; do not assume
  full CPython standard-library behavior.
- Do not claim board compatibility without runtime inspection and relevant
  verification.
- Keep `manifest.py`, `package.json`, and example deployment commands practical
  and current when packaging work is touched.

## MicroPython SDK Standards

- Keep the public API small, likely centered on an explicit `Honch` client
  object.
- Validate public inputs before handing calls to `_honch_core`.
- Accept Python dictionaries for event properties where possible, and convert
  them to MicroPython-compatible JSON before crossing into C.
- Reject malformed or unsupported property values at API boundaries.
- Do not reimplement core ingest, queue, identity, lifecycle, or retry behavior
  in Python.
- Keep memory bounded and configurable.
- Avoid loading the full queue into memory when a streaming or bounded approach
  is practical.
- Keep local persistence durable enough for device restarts, power loss, and
  intermittent connectivity within MicroPython filesystem limits.
- Keep transport, storage, clock, randomness, compression, reset reason,
  battery, and Wi-Fi status behind replaceable boundaries.
- Preserve portability across common MicroPython ports. Do not assume ESP32-only
  modules unless the adapter documents that dependency.
- Use platform-native modules when available, such as `ujson`, `urequests`,
  `network`, `machine`, `os`, and `time`, but isolate imports that are not
  universally present.
- Treat gzip support as a platform capability that must be detected, injected,
  or clearly documented. The shared wire format currently requires gzip.
- Stamp timestamps at event creation time, not flush time.
- Keep event batches capped at 50 events.
- Use explicit retry and backoff behavior matching the shared spec where
  possible.
- Preserve error information. Return or raise errors in a way callers can act
  on consistently.
- Avoid background tasks by default unless the target runtime and product use
  case justify them. Manual `flush()` should be reliable first.
- Keep examples realistic for device firmware: boot, identify, track, flush,
  shutdown or sleep, and persistent queue configuration.

## Security and Privacy Defaults

- No hardcoded production credentials.
- No secret logging.
- Never log API keys, bearer tokens, customer traits, or event payloads.
- Dev and test tools should log summaries, not sensitive payload contents.
- Validate user input before embedding it in payloads.
- Fail closed on invalid configuration.
- Prefer TLS endpoints for real deployments.
- Do not add telemetry, outbound calls, dependency downloads, or package
  publishing steps without clear need and user awareness.
- Treat local queue files and test payloads as potentially sensitive. Avoid
  printing them in normal logs.

## Scope Control

Default to the smallest professional slice that moves the SDK forward.

Do not add these unless explicitly requested:

- Dashboard or cloud analytics product.
- Mobile SDKs.
- BLE, GATT, pairing, or customer transport protocols.
- Firmware-specific integrations beyond small examples or platform adapters.
- Feature flags or experimentation runtime.
- Production credential handling beyond SDK config.
- New third-party dependencies.
- Board-specific production firmware.

When the shared specs describe future architecture, use them for direction, but
implement only the requested milestone.
