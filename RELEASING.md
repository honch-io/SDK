# Releasing the Honch SDK

This repo ships several packages from one tree. The **four SDK ports share a
single version**; the **companion relays version independently**. Releases are
**tag-driven**: a push of a channel tag triggers that channel's publish workflow,
which holds the registry credentials. Nothing publishes from a laptop.

## Versioning model

- The canonical version is `HONCH_SDK_VERSION` in `core/src/honch_internal.h`.
  It is what every device reports on the wire as `$sdk_version`.
- `tools/test_version_consistency.py` (CI: `version-consistency.yml`) enforces
  that every other declaration matches it: the Arduino/esp-idf/micropython
  manifests, the posix CMake project, the wire-fixture generator input, the
  Arduino vendored core header, and the README port-matrix rows.
- The relays (`mobile/react-native-relay`, `mobile/swift-relay`) are **not** part
  of this — they carry their own versions and are released on their own cadence.

## One command: `tools/release.py`

```bash
python3 tools/release.py              # interactive: prompts for the new version
python3 tools/release.py 0.2.3        # propose 0.2.3, still confirms each step
python3 tools/release.py 0.2.3 --yes  # bump + commit + push commit + push tags, no prompts
python3 tools/release.py --bump-only  # edit files + verify only, no git
python3 tools/release.py --tag-only   # (re)push channel tags for the current version
```

It bumps every declaration, re-syncs the Arduino vendored header, regenerates the
wire fixtures, and verifies (version single-source + core-sync + conformance)
before it offers to commit or push anything.

## Cutting an SDK release

1. **Verify on hardware first.** Run the full sweep (`honch-e2e` skill /
   `tools/release_e2e.py`) and get a green bar. A release is not a place to
   discover a regression.
2. **Bump.** On a branch: `python3 tools/release.py 0.2.3`. Review the diff.
3. **Land it.** Open a PR (or push to `main` if that is your flow). `version-
   consistency` + every port's test workflow must pass.
4. **Tag from clean `main`.** The release tool can push the tags for you
   (`--tag-only` after the commit is on `main`), or push them by hand:
   - `arduino-v0.2.3`  → `arduino-publish.yml`     → PlatformIO `honch/Honch`
   - `esp-idf-v0.2.3`  → `esp-idf-publish.yml`      → Espressif registry `honch-io/honch`
   - `micropython-v0.2.3` → `micropython-publish.yml` → PyPI `honch-micropython`
   Push all three at the same version for a coordinated release.

   POSIX has no registry — it ships as source from the tagged tree, so there is
   no publish step (the version bump + README row are enough).

## Releasing a relay (independent version)

The relays change on their own schedule. Bump the package's own version, land it,
then tag:

- `rn-relay-vX.Y.Z`    → `rn-relay-publish.yml`    → npm `@honch/react-native-relay`
- `swift-relay-vX.Y.Z` → GitHub release/tag        → SwiftPM consumers resolve the tag directly

## Hard rules

- **Registry versions are immutable.** A name+version can never be reused, even
  after unpublishing. Always bump; never re-tag an existing version.
- **Publish only from `main`.** Tags must point at a merged, CI-green release
  commit. Each publish workflow re-checks the relevant gate (e.g. Arduino re-runs
  `check-core-sync.sh`) and asserts the tag version matches the manifest before
  uploading.
- **Tag prefixes are the contract** (`arduino-v`, `esp-idf-v`, `micropython-v`,
  `rn-relay-v`, `swift-relay-v`). The wrong prefix triggers the wrong workflow or
  none at all.

## Required CI secrets (org/repo settings)

| Channel | Secret | Notes |
| --- | --- | --- |
| Arduino / PlatformIO | `PLATFORMIO_AUTH_TOKEN` | already configured |
| esp-idf / Espressif | `IDF_COMPONENT_API_TOKEN` | from the Component Registry account |
| micropython / PyPI | `PYPI_API_TOKEN` | or use OIDC trusted publishing (no secret) |
| react-native / npm | `NPM_TOKEN` | automation token with publish rights to `@honch` |

Until a secret is present, that channel's workflow will fail at the publish step —
the bump, version-consistency, and per-port test workflows are unaffected.
