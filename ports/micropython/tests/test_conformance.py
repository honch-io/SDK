import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def load_fixture(path):
    return json.loads((ROOT / path).read_text())


class ConformanceFixtureTests(unittest.TestCase):
    def test_basic_track_fixture_loads(self):
        fixture = load_fixture("spec/conformance/events/basic-track.json")
        self.assertEqual(fixture["name"], "basic track")
        self.assertEqual(fixture["expect"]["distinct_id"], "dev_fixture")

    def test_http_response_policy_fixture_loads(self):
        fixture = load_fixture("spec/conformance/http/response-policy.json")
        self.assertTrue(
            any(case["status"] == 429 and case["queue"] == "preserve" for case in fixture["cases"])
        )


if __name__ == "__main__":
    unittest.main()
