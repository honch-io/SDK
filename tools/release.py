#!/usr/bin/env python3
"""Interactive release tool for the Honch SDK.

The SDK ports (esp-idf, c/posix, micropython, arduino) share ONE version,
`HONCH_SDK_VERSION` in core/src/honch_internal.h -- the value every device
reports on the wire as `$sdk_version`. This tool bumps that version everywhere
it is declared, re-syncs the Arduino vendored core, regenerates the wire
fixtures, verifies with the single-source guard, and then (optionally) commits
and pushes the per-channel tags that trigger the publish workflows.

It never publishes directly: registry credentials live in CI. Pushing a tag
like `arduino-v0.2.3` is what fires `.github/workflows/arduino-publish.yml`.

Usage:
    python3 tools/release.py                 # fully interactive
    python3 tools/release.py 0.2.3           # propose this version, still confirm
    python3 tools/release.py 0.2.3 --yes     # bump + commit + push, no prompts
    python3 tools/release.py --bump-only      # edit files + verify, no git
    python3 tools/release.py --tag-only       # (re)push channel tags for the current version

The companion relay packages (react-native, swift) are versioned independently
and are handled with --relay (see RELEASING.md), not by the shared bump.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CANONICAL_FILE = "core/src/honch_internal.h"
CANONICAL_RE = r'#define HONCH_SDK_VERSION "([^"]+)"'

# (label, path, regex). The capture group is the version literal to rewrite.
# Mirrors tools/test_version_consistency.py -- keep the two in lock-step.
DECLARATIONS = [
    ("canonical core header", CANONICAL_FILE, CANONICAL_RE),
    ("arduino library.properties", "ports/arduino/library.properties", r"^version=(.+)$"),
    ("arduino library.json", "ports/arduino/library.json", r'"version":\s*"([^"]+)"'),
    ("esp-idf root manifest", "idf_component.yml", r'^version:\s*"([^"]+)"'),
    ("esp-idf component manifest", "ports/esp-idf/honch/idf_component.yml", r'^version:\s*"([^"]+)"'),
    ("esp-idf README dependency", "ports/esp-idf/README.md", r'honch/honch\^([0-9][^"\s]+)'),
    ("micropython __init__", "ports/micropython/honch/__init__.py", r'__version__ = "([^"]+)"'),
    ("micropython config", "ports/micropython/honch/config.py", r'SDK_VERSION = "([^"]+)"'),
    ("micropython package.json", "ports/micropython/package.json", r'"version":\s*"([^"]+)"'),
    ("micropython pyproject", "ports/micropython/pyproject.toml", r'^version = "([^"]+)"'),
    ("posix CMake project", "ports/posix/CMakeLists.txt", r"project\(honch_posix_sdk VERSION ([0-9][^\s]+) LANGUAGES C\)"),
    ("wire-fixture generator", "tools/generate_wire_v2_fixtures.py", r'"\$sdk_version": "([^"]+)"'),
    ("http-json reference client", "examples/http-json/typescript/honchClient.ts", r'SDK_VERSION = "([^"]+)"'),
    # Per-port README "Status" lines (the advertised version for each port).
    # Mirrors tools/test_version_consistency.py -- keep the two in lock-step.
    ("arduino README status", "ports/arduino/README.md", r'Preview `([0-9][^`]*)`'),
    ("posix README status", "ports/posix/README.md", r'Stable `([0-9][^`]*)`'),
    ("esp-idf README status", "ports/esp-idf/README.md", r'Stable `([0-9][^`]*)`'),
    ("micropython README status", "ports/micropython/README.md", r'Stable `([0-9][^`]*)`'),
]

# Hand-authored JSON conformance fixtures carry $sdk_version as a literal (often
# more than once per file), so they need a glob rewrite rather than a single
# splice. Mirrors test_json_conformance_fixtures_match_canonical in the guard.
JSON_FIXTURE_DIR = "spec/conformance/json"
JSON_SDK_VERSION_RE = r'("\$sdk_version":\s*")([^"]+)(")'

# README port-matrix rows that advertise a version in a `backtick` cell.
README_FILE = "README.md"
README_ROW_TMPL = r"(\| {label} \|[^|]+\| `)([^`]+)(` \|)"
README_LABELS = ["ESP-IDF", "C/POSIX", "MicroPython", "Arduino ESP32"]

# Vendored core copy that must stay byte-identical (carries the version too).
VENDORED_HEADER = "ports/arduino/src/honch_internal.h"

# Tag prefix per publish channel -> the workflow it triggers.
CHANNELS = {
    "arduino": "arduino-v",      # -> arduino-publish.yml (PlatformIO honch/Honch)
    "esp-idf": "esp-idf-v",      # -> esp-idf-publish.yml (Espressif registry honch/honch)
    "micropython": "micropython-v",  # -> micropython-publish.yml (PyPI)
}

SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.\-]+)?$")


# ---- small terminal helpers (no external deps) ----
def _supports_color() -> bool:
    return sys.stdout.isatty()


def c(text: str, code: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _supports_color() else text


def info(msg: str) -> None:
    print(c("• ", "36") + msg)


def ok(msg: str) -> None:
    print(c("✓ ", "32") + msg)


def warn(msg: str) -> None:
    print(c("! ", "33") + msg)


def die(msg: str) -> None:
    print(c("✗ ", "31") + msg, file=sys.stderr)
    sys.exit(1)


def confirm(prompt: str, assume_yes: bool) -> bool:
    if assume_yes:
        print(f"{prompt} [y/N] y (auto)")
        return True
    try:
        return input(f"{prompt} [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=ROOT, text=True, **kw)


# ---- version reading / editing ----
def read_canonical() -> str:
    text = (ROOT / CANONICAL_FILE).read_text(encoding="utf-8")
    m = re.search(CANONICAL_RE, text)
    if not m:
        die(f"could not find HONCH_SDK_VERSION in {CANONICAL_FILE}")
    return m.group(1)


def _splice_group1(text: str, pattern: str, new: str, path: str) -> str:
    m = re.search(pattern, text, flags=re.MULTILINE)
    if not m:
        die(f"version pattern not found in {path}: {pattern}")
    start, end = m.span(1)
    return text[:start] + new + text[end:]


def bump_declarations(new: str) -> list[str]:
    changed = []
    for label, rel, pattern in DECLARATIONS:
        p = ROOT / rel
        text = p.read_text(encoding="utf-8")
        updated = _splice_group1(text, pattern, new, rel)
        if updated != text:
            p.write_text(updated, encoding="utf-8")
            changed.append(label)
    # README port-matrix rows.
    readme = ROOT / README_FILE
    rtext = readme.read_text(encoding="utf-8")
    for label in README_LABELS:
        pat = README_ROW_TMPL.format(label=re.escape(label))
        rtext, n = re.subn(pat, lambda mm: mm.group(1) + new + mm.group(3), rtext)
        if n == 0:
            die(f"README row not found for {label}")
    readme.write_text(rtext, encoding="utf-8")
    changed.append(f"README rows ({', '.join(README_LABELS)})")
    return changed


def bump_json_fixtures(new: str) -> list[str]:
    changed = []
    for p in sorted((ROOT / JSON_FIXTURE_DIR).glob("*.json")):
        text = p.read_text(encoding="utf-8")
        updated, n = re.subn(
            JSON_SDK_VERSION_RE, lambda mm: mm.group(1) + new + mm.group(3), text
        )
        if n and updated != text:
            p.write_text(updated, encoding="utf-8")
            changed.append(f"json fixture {p.name} ({n}x)")
    return changed


def sync_vendored_header() -> None:
    src = (ROOT / CANONICAL_FILE).read_text(encoding="utf-8")
    (ROOT / VENDORED_HEADER).write_text(src, encoding="utf-8")


# ---- verification ----
def regenerate_fixtures() -> None:
    r = run([sys.executable, "tools/generate_wire_v2_fixtures.py"], capture_output=True)
    if r.returncode != 0:
        die("fixture generation failed:\n" + (r.stderr or r.stdout))


def verify() -> None:
    checks = [
        ("version single-source", [sys.executable, "-m", "unittest", "tools.test_version_consistency"]),
        ("arduino vendored-core sync", ["bash", "ports/arduino/scripts/check-core-sync.sh"]),
        ("wire-v2 conformance", [sys.executable, "-m", "unittest", "spec.conformance.test_wire_v2_fixtures"]),
    ]
    for name, cmd in checks:
        r = run(cmd, capture_output=True)
        if r.returncode != 0:
            die(f"{name} FAILED:\n" + (r.stderr or r.stdout))
        ok(name)


# ---- git ----
def git_clean_on_main() -> tuple[str, bool]:
    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True).stdout.strip()
    dirty = bool(run(["git", "status", "--porcelain"], capture_output=True).stdout.strip())
    return branch, dirty


def git_diff_stat() -> str:
    return run(["git", "diff", "--stat"], capture_output=True).stdout


def git_commit(new: str) -> None:
    run(["git", "add", "-u"])
    run(["git", "commit", "-q", "-m", f"release: bump SDK version to {new}"])


def push_tags(new: str, channels: list[str], assume_yes: bool) -> None:
    for ch in channels:
        tag = f"{CHANNELS[ch]}{new}"
        existing = run(["git", "tag", "-l", tag], capture_output=True).stdout.strip()
        if existing:
            warn(f"tag {tag} already exists locally — skipping (registry versions are immutable)")
            continue
        if not confirm(f"create + push tag {c(tag, '1')} (publishes {ch})?", assume_yes):
            info(f"skipped {ch}")
            continue
        run(["git", "tag", tag])
        r = run(["git", "push", "origin", tag], capture_output=True)
        if r.returncode != 0:
            warn(f"push {tag} failed:\n{r.stderr}")
        else:
            ok(f"pushed {tag} -> CI will publish {ch}")


# ---- main ----
def main() -> int:
    ap = argparse.ArgumentParser(description="Honch SDK release tool")
    ap.add_argument("version", nargs="?", help="new version (x.y.z); prompted if omitted")
    ap.add_argument("--yes", action="store_true", help="no prompts (commit + push all channels)")
    ap.add_argument("--bump-only", action="store_true", help="edit files + verify, no git")
    ap.add_argument("--tag-only", action="store_true", help="push channel tags for the current version, skip bump")
    ap.add_argument("--channels", default=",".join(CHANNELS), help="comma list: " + ",".join(CHANNELS))
    args = ap.parse_args()

    channels = [x.strip() for x in args.channels.split(",") if x.strip()]
    for ch in channels:
        if ch not in CHANNELS:
            die(f"unknown channel {ch!r}; valid: {', '.join(CHANNELS)}")

    current = read_canonical()
    print(c("\nHonch SDK release", "1"))
    info(f"current version: {c(current, '1')}")

    if args.tag_only:
        branch, dirty = git_clean_on_main()
        if branch != "main":
            warn(f"on branch {branch!r}, not main — tags should point at a merged release commit")
        push_tags(current, channels, args.yes)
        return 0

    new = args.version or (current if args.yes else input(f"new version (current {current}): ").strip())
    if not SEMVER_RE.match(new):
        die(f"{new!r} is not semver x.y.z")
    if new == current:
        die("new version equals current — bump it (registry versions are immutable)")

    branch, dirty = git_clean_on_main()
    if dirty and not args.bump_only:
        die("working tree is dirty — commit or stash first")
    if branch != "main":
        warn(f"on branch {branch!r}, not main")

    info(f"bumping {current} -> {c(new, '1')}")
    changed = bump_declarations(new)
    changed += bump_json_fixtures(new)
    sync_vendored_header()
    for label in changed:
        ok(f"updated {label}")
    ok("re-synced arduino vendored header")
    regenerate_fixtures()
    ok("regenerated wire fixtures")

    print()
    info("verifying...")
    verify()

    print(c("\nchanges:", "1"))
    print(git_diff_stat())

    if args.bump_only:
        info("--bump-only: stopping before git. Review the diff, then commit/tag yourself or re-run.")
        return 0

    if not confirm(f"commit as 'release: bump SDK version to {new}'?", args.yes):
        info("left changes uncommitted; nothing pushed")
        return 0
    git_commit(new)
    ok(f"committed release bump {new}")

    warn("Push the release COMMIT to main (and let CI pass) before pushing tags.")
    if not confirm("push the commit to origin/main now?", args.yes):
        info("commit not pushed; re-run with --tag-only after it lands on main")
        return 0
    pr = run(["git", "push", "origin", "HEAD:main"], capture_output=True)
    if pr.returncode != 0:
        die(f"push failed:\n{pr.stderr}")
    ok("pushed release commit to main")

    push_tags(new, channels, args.yes)
    print(c("\ndone. CI publishes each channel; registry versions are immutable.", "32"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
