# AGENTS.md

You are working inside the Honch C/POSIX SDK package in the Honch SDK monorepo.

Act like a precise, concise, direct, safety-minded, and solution-oriented agent.

## Product North Star

Honch is analytics infrastructure for consumer hardware companies. Customers embed Honch SDKs into firmware, embedded Linux software, or companion apps so their product teams can analyze funnels, cohorts, retention, firmware behavior, and experiments without building a telemetry pipeline.

The monorepo is for SDKs, not the full SaaS product. Do not expand work into the dashboard, cloud ingest service, BLE protocols, firmware integrations, billing, auth, or analytics UI unless explicitly requested.

The cross-SDK source of truth lives in `../spec/`. Treat local C/POSIX docs as package guidance, not as a replacement for the shared spec.

## Current Focus

The current package is the C/POSIX SDK:

- Build with CMake on macOS and Linux.
- Provide a production-shaped public C API.
- Persist SDK state and queued events locally before delivery.
- Keep encoder and transport code isolated so the package can move cleanly to the shared CBOR ingest contract when the org-level API/spec update lands.
- Keep the local mock collector and JSON development path only as a temporary development harness until the shared ingest contract is finalized.
- Include a local mock collector for smoke testing.
- Include deterministic tests for queueing, validation, batching, retry, reset, persistence, and dead-letter behavior.

The C core should stay reusable for later Embedded Linux, Zephyr, Arduino, bare-metal C, and MicroPython work. Keep platform-specific behavior behind replaceable boundaries.

## Package Shape

- `include/honch`: Public C headers.
- `src`: C11 SDK implementation and internal modules.
- `tests`: C test executable.
- `examples/posix_device`: Small example app that simulates a connected device.
- `examples/connected_camera`: Realistic hardware-style example.
- `tools/mock_collector.py`: Local HTTP collector for smoke testing.
- `../spec`: Shared cross-SDK contract and conformance fixtures. Read these before large SDK changes.

Do not mix other platform SDKs into this package. Future SDKs should live as sibling root directories in the monorepo.

## Required Workflow

Before changing files:

- Run `git status --short`.
- Identify whether existing changes are yours or user-owned. Preserve all user-owned changes.
- Read the relevant docs and code before editing.
- For contract-sensitive changes, read `../spec/wire-format.md`, `../spec/auto-properties.md`, and any relevant conformance fixtures.
- For non-trivial work, state the plan briefly before edits.

During work:

- Keep changes scoped to the requested task.
- Prefer simple, maintainable code over clever abstractions.
- Do not vendor dependencies without clear justification.
- Do not introduce unrelated refactors.
- Update tests and docs in the same change when behavior or public API changes.
- Stop and call out incorrect assumptions, unsafe requests, or scope creep.

Before reporting completion:

- Run the relevant verification commands.
- Report exactly what was verified.
- Report any tests or checks that could not be run.
- Leave generated build artifacts untracked.

## Git Policy

This is a real project. Treat git safety as part of the job.

- Never use destructive git commands such as `git reset --hard`, `git checkout --`, `git clean`, branch deletion, or force-push unless explicitly requested.
- Never overwrite unrelated user changes.
- Do not commit unless explicitly requested.
- Do not amend, rebase, squash, or rewrite history unless explicitly requested.
- If the worktree is dirty, inspect and work around unrelated changes.
- If a file you need to edit has user changes, preserve them and adapt your patch.

Branch policy:

- Once the repository has an initial commit, do substantive work on a feature branch, not directly on `main`.
- Use clear branch names such as `feat/c-core-queue`, `fix/json-validation`, or `docs/agent-rules`.
- Before creating a branch, check the current branch and worktree state.
- If the repo has no commits yet, do not invent history. Work in place until the user asks to create the initial commit.
- Keep commits focused when the user asks for commits: one coherent change per commit, with tests passing.

Suggested commit message style when commits are requested:

- `feat(c-core): add persistent device identity`
- `fix(c-core): preserve queued events on retry`
- `test(c-core): cover reset and dead-letter behavior`
- `docs: tighten agent workflow rules`

## Quality Gates

Every behavior change needs concrete verification.

Default C/POSIX verification:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For Python tooling changes:

```sh
python3 -m py_compile tools/mock_collector.py
```

Testing standards:

- Add or update tests for every SDK behavior change.
- Prefer deterministic tests with fake transports and local temp directories.
- Unit tests must not require real network access.
- Test success, validation failure, retryable failure, permanent rejection, persistence across restart, and bounded queue behavior when touched.
- Do not claim a behavior works unless a test, smoke test, or direct inspection proves it.

Build standards:

- Treat warnings as defects.
- Keep the C SDK warning-clean under the configured compiler flags.
- Do not silence warnings without fixing the underlying issue.

## C SDK Standards

- Write C11-compatible code.
- Use typed status codes; do not hide failures.
- Validate all public API inputs.
- Keep user-provided property strings valid JSON while the public C API accepts JSON-shaped property input.
- Reject malformed JSON at API boundaries.
- Do not hard-code the current ingest API shape into core logic. The C/POSIX package is expected to move to CBOR when the shared API/spec update is ready.
- Use explicit ownership rules for allocated memory.
- Free all owned memory on every error path.
- Keep memory bounded and configurable.
- Avoid process-wide mutable globals unless there is a clear reason.
- Keep transport, storage, time, and randomness behind replaceable boundaries.
- Preserve portability: core logic should not depend directly on POSIX where an adapter can own that detail.
- Keep public API changes intentional and documented in `README.md`.
- Treat SDK behavior consistency across platforms as a product requirement.

## Security and Privacy Defaults

- No hardcoded production credentials.
- No secret logging.
- Never log API keys, bearer tokens, customer traits, or event payloads.
- Mock/dev tools should log summaries, not sensitive payload contents.
- Validate JSON-shaped user input before embedding it in payloads.
- Fail closed on invalid configuration.
- Prefer TLS endpoints for real deployments.
- Do not add telemetry, outbound calls, or dependency downloads without clear need and user awareness.

## Scope Control

Default to the smallest professional slice that moves the SDK forward.

Do not add these unless explicitly requested:

- Dashboard or cloud analytics product.
- Mobile SDKs.
- BLE, GATT, pairing, or customer transport protocols.
- Firmware-specific integrations.
- Feature flags or experimentation runtime.
- Production credential handling beyond SDK config.
- New third-party dependencies.

When the shared specs describe future architecture, use them for direction, but implement only the requested milestone.
