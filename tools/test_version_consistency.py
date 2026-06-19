"""Single-source-of-truth guard for the SDK version.

``HONCH_SDK_VERSION`` (core/src/honch_internal.h) is the canonical version: it is
what every device reports on the wire as ``$sdk_version``. Every other place the
version is declared -- vendored core, per-port package metadata, the wire-fixture
generator input, and the README tables -- must match it exactly.

This catches the drift class where a port package is bumped (e.g. an Arduino
metadata-only release) without bumping the shared version, leaving devices
reporting a stale ``$sdk_version`` and stale READMEs/manifests shipping.

To cut a release: bump ``HONCH_SDK_VERSION`` (and its byte-identical vendored
copy), then update every declaration this test checks until it passes.

Run from the repo root:
    python3 -m unittest tools.test_version_consistency
"""

import glob
import json
import os
import re
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Canonical declaration: the wire-reported SDK version.
CANONICAL_FILE = "core/src/honch_internal.h"
CANONICAL_RE = r'#define HONCH_SDK_VERSION "([^"]+)"'

# (label, relative path, regex with one capture group that must equal canonical)
DECLARATIONS = [
    ("vendored arduino core macro", "ports/arduino/src/honch_internal.h", CANONICAL_RE),
    ("arduino library.properties", "ports/arduino/library.properties", r"^version=(.+)$"),
    ("arduino library.json", "ports/arduino/library.json", r'"version":\s*"([^"]+)"'),
    ("esp-idf root manifest", "idf_component.yml", r'^version:\s*"([^"]+)"'),
    ("esp-idf component manifest", "ports/esp-idf/honch/idf_component.yml", r'^version:\s*"([^"]+)"'),
    ("esp-idf README add-dependency", "ports/esp-idf/README.md", r'honch/honch\^([0-9][^"\s]+)'),
    ("micropython __init__", "ports/micropython/honch/__init__.py", r'__version__ = "([^"]+)"'),
    ("micropython config SDK_VERSION", "ports/micropython/honch/config.py", r'SDK_VERSION = "([^"]+)"'),
    ("micropython package.json", "ports/micropython/package.json", r'"version":\s*"([^"]+)"'),
    ("micropython pyproject", "ports/micropython/pyproject.toml", r'^version = "([^"]+)"'),
    ("posix CMake project", "ports/posix/CMakeLists.txt", r"project\(honch_posix_sdk VERSION ([0-9][^\s]+) LANGUAGES C\)"),
    ("wire-fixture generator input", "tools/generate_wire_v2_fixtures.py", r'"\$sdk_version": "([^"]+)"'),
    ("http-json reference client", "examples/http-json/typescript/honchClient.ts", r'SDK_VERSION = "([^"]+)"'),
    # Per-port README "Status" lines (the advertised version for each port).
    ("arduino README status", "ports/arduino/README.md", r'Preview `([0-9][^`]*)`'),
    ("posix README status", "ports/posix/README.md", r'Stable `([0-9][^`]*)`'),
    ("esp-idf README status", "ports/esp-idf/README.md", r'Stable `([0-9][^`]*)`'),
    ("micropython README status", "ports/micropython/README.md", r'Stable `([0-9][^`]*)`'),
]

# The JSON conformance fixtures are hand-authored (not generated like wire-v2),
# so each carries $sdk_version as a literal, sometimes more than once per file.
# They must track canonical too: the valid ones expand to the same canonical
# events as the wire-v2 fixtures, which only holds if $sdk_version matches.
# Checked by glob rather than a single DECLARATIONS regex because of the many
# occurrences. (tools/release.py mirrors this in bump_json_fixtures -- keep the
# two in lock-step.)
JSON_FIXTURE_GLOB = "spec/conformance/json/*.json"
JSON_SDK_VERSION_RE = r'"\$sdk_version":\s*"([^"]+)"'

# README port-matrix rows that advertise a port version.
README_ROW_RE = r"\| {label} \|[^|]+\| `([^`]+)` \|"
README_ROWS = ["ESP-IDF", "C/POSIX", "MicroPython", "Arduino ESP32"]


def read(rel_path):
    with open(os.path.join(REPO_ROOT, rel_path), "r", encoding="utf-8") as handle:
        return handle.read()


class VersionConsistencyTest(unittest.TestCase):
    def setUp(self):
        match = re.search(CANONICAL_RE, read(CANONICAL_FILE), re.MULTILINE)
        self.assertIsNotNone(match, "HONCH_SDK_VERSION not found in " + CANONICAL_FILE)
        self.canonical = match.group(1)

    def test_canonical_version_is_semver(self):
        self.assertRegex(self.canonical, r"^\d+\.\d+\.\d+$")

    def test_all_declarations_match_canonical(self):
        mismatches = []
        for label, rel_path, pattern in DECLARATIONS:
            match = re.search(pattern, read(rel_path), re.MULTILINE)
            if match is None:
                mismatches.append("%s: pattern not found in %s" % (label, rel_path))
            elif match.group(1) != self.canonical:
                mismatches.append(
                    "%s (%s): %r != canonical %r"
                    % (label, rel_path, match.group(1), self.canonical)
                )
        self.assertEqual([], mismatches, "version drift:\n" + "\n".join(mismatches))

    def test_json_conformance_fixtures_match_canonical(self):
        paths = sorted(glob.glob(os.path.join(REPO_ROOT, JSON_FIXTURE_GLOB)))
        self.assertTrue(paths, "no JSON conformance fixtures found at " + JSON_FIXTURE_GLOB)
        mismatches = []
        occurrences = 0
        for path in paths:
            rel = os.path.relpath(path, REPO_ROOT)
            for match in re.finditer(JSON_SDK_VERSION_RE, read(rel)):
                occurrences += 1
                if match.group(1) != self.canonical:
                    mismatches.append(
                        "%s: %r != canonical %r" % (rel, match.group(1), self.canonical)
                    )
        self.assertGreater(occurrences, 0, "no $sdk_version found in JSON fixtures")
        self.assertEqual([], mismatches, "JSON fixture version drift:\n" + "\n".join(mismatches))

    def test_readme_port_matrix_matches_canonical(self):
        readme = read("README.md")
        mismatches = []
        for label in README_ROWS:
            match = re.search(README_ROW_RE.format(label=re.escape(label)), readme)
            if match is None:
                mismatches.append("README row not found: " + label)
            elif match.group(1) != self.canonical:
                mismatches.append(
                    "README %s row: %r != canonical %r"
                    % (label, match.group(1), self.canonical)
                )
        self.assertEqual([], mismatches, "README version drift:\n" + "\n".join(mismatches))

    def test_arduino_library_json_is_valid(self):
        # library.json is also parsed by the registry; keep it valid JSON.
        data = json.loads(read("ports/arduino/library.json"))
        self.assertEqual(self.canonical, data["version"])


if __name__ == "__main__":
    unittest.main()
