"""Structural sanity gate for every conformance fixture.

The cross-SDK conformance fixtures under spec/conformance/ are the contract each
SDK validates against. Only the wire-v2 fixtures have a Python runner
(test_wire_v2_fixtures.py); the events/, http/, and relay/ fixtures are consumed
by per-language SDK conformance runners and otherwise have no CI coverage here.

This test does not assert semantics -- it guards against the cheap failure modes
that would silently rot the contract: a fixture that stops being valid JSON, an
empty fixture, or a whole fixture directory disappearing (e.g. a bad glob or
merge). Semantic validation of events/http/relay belongs in their own runners.

Run from the repo root:
    python3 -m unittest spec.conformance.test_fixtures_valid
"""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
# Every subdirectory that must always contain at least one fixture.
FIXTURE_DIRS = ["events", "http", "relay", "wire-v2"]


class FixturesValidTest(unittest.TestCase):
    def test_every_fixture_is_nonempty_json_object(self):
        fixtures = sorted(ROOT.glob("**/*.json"))
        self.assertGreater(len(fixtures), 0, "no conformance fixtures found")
        for path in fixtures:
            rel = path.relative_to(ROOT)
            with self.subTest(fixture=str(rel)):
                try:
                    data = json.loads(path.read_text(encoding="utf-8"))
                except json.JSONDecodeError as exc:
                    self.fail("%s is not valid JSON: %s" % (rel, exc))
                self.assertIsInstance(data, dict, "%s is not a JSON object" % rel)
                self.assertGreater(len(data), 0, "%s is an empty object" % rel)

    def test_each_fixture_dir_is_populated(self):
        for name in FIXTURE_DIRS:
            directory = ROOT / name
            with self.subTest(directory=name):
                self.assertTrue(directory.is_dir(), "missing fixture dir: " + name)
                self.assertGreater(
                    len(list(directory.glob("*.json"))),
                    0,
                    "no fixtures in spec/conformance/" + name,
                )


if __name__ == "__main__":
    unittest.main()
