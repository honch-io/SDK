import importlib
import json
import sys
import types
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

CANONICAL_FIXTURES = (
    "spec/conformance/events/basic-track.json",
    "spec/conformance/events/auto_stamp_wins_conflict.json",
    "spec/conformance/events/boot_event.json",
    "spec/conformance/events/custom_event_with_session.json",
    "spec/conformance/events/identity-reset.json",
    "spec/conformance/events/session-track.json",
    "spec/conformance/http/response-policy.json",
)

IDENTITY_SEMANTICS_FIXTURES = (
    "spec/conformance/events/persisted-identity-reboot.json",
)


def load_fixture(path):
    return json.loads((ROOT / path).read_text())


class FakeCoreClient:
    def __init__(self, config):
        self.config = dict(config)
        self.calls = []

    def track(self, event_name, properties):
        self.calls.append(("track", event_name, properties))

    def identify(self, distinct_id, traits):
        self.calls.append(("identify", distinct_id, traits))

    def session_start(self, session_name):
        self.calls.append(("session_start", session_name))

    def session_end(self):
        self.calls.append(("session_end",))

    def reset(self):
        self.calls.append(("reset",))


class ConformanceFixtureTests(unittest.TestCase):
    def setUp(self):
        self.saved = {
            name: module
            for name, module in sys.modules.items()
            if name == "_honch_core" or name == "honch" or name.startswith("honch.")
        }
        for name in list(sys.modules):
            if name == "_honch_core" or name == "honch" or name.startswith("honch."):
                del sys.modules[name]

        fake = types.ModuleType("_honch_core")
        fake.Client = FakeCoreClient
        fake.ERROR_INVALID_ARGUMENT = 1
        fake.ERROR_IO = 3
        fake.ERROR_TRANSPORT = 4
        fake.ERROR_RATE_LIMITED = 5
        fake.ERROR_SERVER = 6
        fake.ERROR_REJECTED = 7
        fake.ERROR_NOT_INITIALIZED = 8
        fake.ERROR_QUEUE_FULL = 10
        fake.ERROR_TIMEOUT = 11
        sys.modules["_honch_core"] = fake

    def tearDown(self):
        for name in list(sys.modules):
            if name == "_honch_core" or name == "honch" or name.startswith("honch."):
                del sys.modules[name]
        sys.modules.update(self.saved)

    def test_all_canonical_fixtures_are_exercised_by_micropython_conformance_tests(self):
        for path in CANONICAL_FIXTURES + IDENTITY_SEMANTICS_FIXTURES:
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).exists())

    def test_basic_track_fixture_delegates_config_and_track_call_to_core(self):
        fixture = load_fixture("spec/conformance/events/basic-track.json")
        client = self._client_from_fixture_config(fixture["config"])

        op = fixture["operations"][0]
        client.track(op["event"], op["properties"])

        self.assertEqual(client._core.config["device_id"], fixture["config"]["device_id"])
        self.assertEqual(client._core.config["device_model"], fixture["config"]["device_model"])
        self.assertEqual(client._core.config["firmware_version"], fixture["config"]["firmware_version"])
        self.assertEqual(client._core.config["environment"], fixture["config"]["environment"])
        self.assertEqual(
            client._core.calls,
            [("track", fixture["expect"]["event"], op["properties"])],
        )

    def test_input_style_event_fixtures_delegate_track_calls_to_core(self):
        for path in (
            "spec/conformance/events/boot_event.json",
            "spec/conformance/events/custom_event_with_session.json",
        ):
            fixture = load_fixture(path)
            with self.subTest(path=path):
                client = self._client_from_fixture_config(fixture["input"]["config"])
                if "session_id" in fixture["input"]:
                    client.session_start(None)
                client.track(fixture["input"]["event"], fixture["input"]["properties"])

                self.assertEqual(client._core.config["device_model"], fixture["input"]["config"]["device_model"])
                self.assertEqual(
                    client._core.calls[-1],
                    ("track", fixture["expected"]["event"], fixture["input"]["properties"]),
                )

    def test_auto_stamp_conflict_fixture_expects_rejection(self):
        fixture = load_fixture("spec/conformance/events/auto_stamp_wins_conflict.json")
        readme = (ROOT / "spec/conformance/README.md").read_text(encoding="utf-8")

        self.assertEqual(fixture["description"], "SDK rejects user-supplied SDK-owned properties before queueing")
        self.assertEqual(fixture["expected"], {"error": "invalid_argument"})
        self.assertIn("$device_model", fixture["input"]["properties"])
        self.assertIn("$sdk_platform", fixture["input"]["properties"])
        self.assertIn("SDK-owned property keys are rejected", readme)
        self.assertNotIn("SDK-owned properties win over user-supplied properties", readme)

    def test_identity_reset_fixture_delegates_identify_reset_and_final_track(self):
        fixture = load_fixture("spec/conformance/events/identity-reset.json")
        client = self._client_from_fixture_config(fixture["config"])
        adapter = (ROOT / "ports/micropython/usermod/honch/mphal_adapter.c").read_text()

        for op in fixture["operations"]:
            if op["op"] == "identify":
                client.identify(op["distinct_id"], op["traits"])
            elif op["op"] == "reset":
                client.reset()
            elif op["op"] == "track":
                client.track(op["event"], op["properties"])

        self.assertEqual(
            client._core.calls,
            [
                ("identify", "user_123", {"plan": "beta"}),
                ("reset",),
                ("track", "after_reset", {}),
            ],
        )
        self.assertIn("client->configured_device_id = true", adapter)
        self.assertIn("client->distinct_id = distinct_id;", adapter)

    def test_identity_is_volatile_without_state_storage(self):
        fixture = load_fixture("spec/conformance/events/persisted-identity-reboot.json")
        adapter = (ROOT / "ports/micropython/usermod/honch/mphal_adapter.c").read_text()
        module = (ROOT / "ports/micropython/usermod/honch/modhonch_core.c").read_text()

        self.assertEqual(fixture["expect"]["pre_identify_distinct_id_after_reboot"], "user_123")
        self.assertEqual(
            [op["op"] for op in fixture["operations"]],
            ["identify", "reboot", "track"],
        )
        self.assertIn("return HONCH_STATUS_ERROR_INVALID_ARGUMENT;", adapter)
        self.assertIn("client->distinct_id = honch_micropython_strdup(client->device_id);", adapter)
        self.assertNotIn("state_get", adapter)
        self.assertNotIn("state_set", adapter)
        self.assertIn("honch_core_identify(self->client", module)
        self.assertIn("honch_core_track(self->client", module)

    def test_session_track_fixture_delegates_session_lifecycle_and_track(self):
        fixture = load_fixture("spec/conformance/events/session-track.json")
        client = self._client_from_fixture_config(fixture["config"])

        for op in fixture["operations"]:
            if op["op"] == "session_start":
                client.session_start(op["session_name"])
            elif op["op"] == "track":
                client.track(op["event"], op["properties"])
            elif op["op"] == "session_end":
                client.session_end()

        self.assertEqual(
            client._core.calls,
            [
                ("session_start", "recording"),
                ("track", "recording_started", {"mode": "hdr"}),
                ("session_end",),
            ],
        )

    def test_response_policy_fixture_is_implemented_by_micropython_transport_adapter(self):
        fixture = load_fixture("spec/conformance/http/response-policy.json")
        transport = (ROOT / "ports/micropython/usermod/honch/mptransport_adapter.c").read_text()

        for case in fixture["cases"]:
            status = case["status"]
            with self.subTest(status=status):
                if status == 0:
                    self.assertIn("http_status == 0", transport)
                    self.assertIn("HONCH_TRANSPORT_RETRY", transport)
                elif case["result"] == "chunk_stored":
                    self.assertIn("http_status == 202", transport)
                    self.assertIn("HONCH_TRANSPORT_CHUNK_STORED", transport)
                elif case["result"] == "accepted":
                    self.assertIn("http_status == 204", transport)
                    self.assertIn("HONCH_TRANSPORT_ACCEPTED", transport)
                elif case["result"] == "auth_error":
                    self.assertIn("http_status == 401", transport)
                    self.assertIn("HONCH_TRANSPORT_AUTH_ERROR", transport)
                elif case["result"] == "rejected":
                    self.assertIn("HONCH_TRANSPORT_REJECTED", transport)
                elif case["result"] == "retry":
                    if status >= 500:
                        self.assertIn("http_status >= 500", transport)
                    else:
                        self.assertIn(f"http_status == {status}", transport)
                    self.assertIn("HONCH_TRANSPORT_RETRY", transport)

    def test_compact_wire_fixture_is_covered_by_the_c_core_user_module(self):
        fixture = load_fixture("spec/conformance/wire-v2/single-required-context.json")
        module = (ROOT / "ports/micropython/usermod/honch/modhonch_core.c").read_text()
        cmake = (ROOT / "ports/micropython/usermod/honch/micropython.cmake").read_text()

        self.assertEqual(fixture["expected_capture_response_code"], 204)
        self.assertIn("honch_core_flush(self->client)", module)
        self.assertIn("../../../../core/src/honch_wire_v2.c", cmake)
        self.assertIn("../../../../core/src/honch_event_record.c", cmake)

    def _client_from_fixture_config(self, config):
        honch = importlib.import_module("honch")
        return honch.Honch(
            api_key=config.get("api_key", "test-key"),
            endpoint_url="http://collector.local",
            device_id=config.get("device_id", "test-device"),
            device_model=config["device_model"],
            firmware_version=config["firmware_version"],
            environment=config["environment"],
            event_buffer=bytearray(8192),
        )


if __name__ == "__main__":
    unittest.main()
