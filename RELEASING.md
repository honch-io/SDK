# Releasing the Honch SDK

Releases are **tag-driven**: pushing a channel tag triggers that channel's
publish workflow in CI, which holds the registry credentials. Nothing publishes
from a laptop.

## What ships, and how

| Artifact | Version source | Published to | Release trigger |
| --- | --- | --- | --- |
| ESP-IDF port | `HONCH_SDK_VERSION` (shared) | Espressif registry `honch/honch` | tag `esp-idf-vX.Y.Z` |
| Arduino port | `HONCH_SDK_VERSION` (shared) | PlatformIO `honch/Honch` | tag `arduino-vX.Y.Z` |
| MicroPython port | `HONCH_SDK_VERSION` (shared) | PyPI `honch-micropython` | tag `micropython-vX.Y.Z` |
| C/POSIX port | `HONCH_SDK_VERSION` (shared) | source only (no registry) | the version bump itself |
| react-native relay | its own `package.json` | npm `@honch/react-native-relay` | tag `rn-relay-vX.Y.Z` |
| swift relay | its own git tag | SwiftPM (git) | tag `swift-relay-vX.Y.Z` |

The **four SDK ports share one version** (`HONCH_SDK_VERSION` in
`core/src/honch_internal.h` — what every device reports on the wire as
`$sdk_version`). `tools/release.py` bumps and releases all four together.

The **relays are separate packages with their own version numbers** and are
released on their own cadence — `release.py` does **not** touch them.

---

## Releasing the SDK ports — `tools/release.py`

This one command covers all four ports. It bumps the shared version everywhere
it is declared, re-syncs the Arduino vendored header, regenerates the wire
fixtures, verifies (version single-source + core-sync + conformance), and can
commit and push the channel tags that trigger publishing.

```bash
python3 tools/release.py              # interactive: prompts for the new version
python3 tools/release.py 0.2.3        # propose 0.2.3, confirm each step
python3 tools/release.py 0.2.3 --yes  # bump + commit + push commit + push all tags, no prompts
python3 tools/release.py --bump-only  # edit files + verify only, no git (review the diff)
python3 tools/release.py --tag-only   # (re)push channel tags for the CURRENT version
```

**Full release, step by step:**

1. **Verify on hardware first.** Run the full sweep (`honch-e2e` skill /
   `tools/release_e2e.py`) and get a green bar. A release is not where you want
   to discover a regression.
2. **Bump.** On a branch: `python3 tools/release.py 0.2.3` and review the diff.
3. **Land it.** PR (or push to `main`). `version-consistency` + every port's test
   workflow must pass.
4. **Tag from clean `main`.** Easiest: `python3 tools/release.py --tag-only`
   (it pushes `arduino-v`, `esp-idf-v`, `micropython-v` for the current version).
   Or push them by hand. Each tag fires its publish workflow.

POSIX has no registry — it ships as source from the tagged tree, so the version
bump + README row are the whole "release" for it.

To release only some channels: `python3 tools/release.py --tag-only --channels arduino,esp-idf`.

---

## Releasing the react-native relay (npm, independent version)

The relay is `@honch/react-native-relay` and versions on its own — it is **not**
tied to `HONCH_SDK_VERSION`.

**One-time prerequisites:**
- The `@honch` npm scope exists and the publisher has publish rights to it.
- The `NPM_TOKEN` repo secret is set (automation token with `@honch` publish rights).

**To release:**
1. Bump `"version"` in `mobile/react-native-relay/package.json` (its own number).
2. Commit and land on `main`.
3. Tag and push:
   ```bash
   git tag rn-relay-v0.1.0 && git push origin rn-relay-v0.1.0
   ```
   `rn-relay-publish.yml` checks the tag matches `package.json`, typechecks, and
   runs `npm publish --access public` (scoped packages need `--access public` on
   first publish).

**Manual fallback** (if not using CI):
```bash
cd mobile/react-native-relay
npm login
npm publish --access public
```

The published tarball is trimmed by `.npmignore` to source + native module +
example (no `build/`, `.gradle/`, `local.properties`, or `AGENTS.md`). Preview it
anytime with `npm pack --dry-run`.

---

## Releasing the swift relay (SwiftPM, independent version)

SwiftPM has no central registry — consumers resolve a **git tag** directly.

1. Bump any version reference in `mobile/swift-relay` if present.
2. Commit and land on `main`.
3. Tag and push, then cut a GitHub Release:
   ```bash
   git tag swift-relay-v0.1.0 && git push origin swift-relay-v0.1.0
   ```

> **Open item to settle before the first swift release:** SwiftPM resolves a
> package from `Package.swift` at a repo root, but this relay lives in a monorepo
> subdirectory (`mobile/swift-relay`), and our tag is prefixed (`swift-relay-v*`),
> which SwiftPM will not read as a plain semver package version. Decide the
> distribution path first — a mirror repo, a root manifest, or documented
> `.package(url:branch:)` consumption — and document the consumer snippet here.

---

## Hard rules

- **Registry versions are immutable.** A name+version can never be reused, even
  after unpublishing. Always bump; never re-tag an existing version.
- **Publish only from `main`.** Tags must point at a merged, CI-green commit.
  Each publish workflow re-checks the relevant gate (e.g. Arduino re-runs
  `check-core-sync.sh`) and asserts the tag version matches the manifest.
- **Tag prefixes are the contract** (`arduino-v`, `esp-idf-v`, `micropython-v`,
  `rn-relay-v`, `swift-relay-v`). The wrong prefix triggers the wrong workflow,
  or none.

## Required CI secrets (org/repo settings)

| Channel | Secret | Notes |
| --- | --- | --- |
| Arduino / PlatformIO | `PLATFORMIO_AUTH_TOKEN` | already configured |
| esp-idf / Espressif | `IDF_COMPONENT_API_TOKEN` | from the Component Registry account |
| micropython / PyPI | `PYPI_API_TOKEN` | or OIDC trusted publishing (no secret) |
| react-native / npm | `NPM_TOKEN` | automation token, `@honch` publish rights |

Until a secret is present, that channel's publish workflow fails at the upload
step only — the bump, version-consistency, and per-port test workflows are
unaffected.
