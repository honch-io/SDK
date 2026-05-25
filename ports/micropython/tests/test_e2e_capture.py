import os
import tempfile
import time
import unittest
import urllib.parse
import urllib.request

import honch

try:
    import _honch_core  # noqa: F401
except ImportError:
    HAS_C_CORE = False
else:
    HAS_C_CORE = True


DEFAULT_CAPTURE_URL = "http://127.0.0.1:8001"
DEFAULT_CAPTURE_HEALTH_URL = "http://127.0.0.1:8001/health"
DEFAULT_WORKER_HEALTH_URL = "http://127.0.0.1:8080/"
DEFAULT_TOKEN = "honch_e2e_test_key"
DEFAULT_PROJECT_ID = "00000000-0000-0000-0000-000000000002"
DEFAULT_CLICKHOUSE_URL = "http://127.0.0.1:8123"
DEFAULT_CLICKHOUSE_DATABASE = "platform"


E2E_ENABLED = os.environ.get("HONCH_E2E") == "1"


def env_or_default(name, default):
    value = os.environ.get(name)
    return value if value else default


def http_get_ok(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        body = response.read()
        if response.status < 200 or response.status >= 300:
            raise AssertionError("health check failed for %s: %s %r" % (url, response.status, body))


def clickhouse_query(clickhouse_url, query):
    encoded = urllib.parse.urlencode({"query": query})
    url = "%s/?%s" % (clickhouse_url.rstrip("/"), encoded)
    with urllib.request.urlopen(url, timeout=5) as response:
        body = response.read().decode("utf-8")
        if response.status < 200 or response.status >= 300:
            raise AssertionError("ClickHouse query failed: %s %s" % (response.status, body))
        return body


def parse_row(line):
    fields = line.rstrip("\n").split("\t")
    while len(fields) < 9:
        fields.append("")
    names = (
        "event",
        "distinct_id",
        "properties",
        "device_id",
        "device_model",
        "firmware_version",
        "session_id",
        "sdk_platform",
        "environment",
    )
    row = {}
    for name, value in zip(names, fields):
        row[name] = "" if value == "\\N" else value
    return row


def clickhouse_fetch_event(clickhouse_url, database, project_id, event_name):
    escaped_event = event_name.replace("'", "\\'")
    query = (
        "SELECT event, distinct_id, properties, ifNull(device_id, ''), "
        "ifNull(device_model, ''), ifNull(firmware_version, ''), "
        "ifNull(session_id, ''), ifNull(sdk_platform, ''), ifNull(environment, '') "
        "FROM %s.events WHERE team_id = '%s' AND event = '%s' "
        "ORDER BY received_at DESC LIMIT 1 FORMAT TabSeparated"
        % (database, project_id, escaped_event)
    )
    body = clickhouse_query(clickhouse_url, query)
    if not body:
        return None
    return parse_row(body)


def wait_for_clickhouse_event(clickhouse_url, database, project_id, event_name):
    for _ in range(45):
        row = clickhouse_fetch_event(clickhouse_url, database, project_id, event_name)
        if row is not None and row.get("event"):
            return row
        time.sleep(1)
    raise AssertionError("E2E event did not appear in ClickHouse: %s" % event_name)


@unittest.skipUnless(E2E_ENABLED and HAS_C_CORE, "set HONCH_E2E=1 and run with _honch_core to run real capture E2E tests")
class HonchCaptureE2ETests(unittest.TestCase):
    def setUp(self):
        self.endpoint = env_or_default("HONCH_E2E_ENDPOINT", DEFAULT_CAPTURE_URL)
        self.token = env_or_default("HONCH_E2E_TOKEN", DEFAULT_TOKEN)
        self.capture_health = env_or_default("HONCH_E2E_CAPTURE_HEALTH_URL", DEFAULT_CAPTURE_HEALTH_URL)
        self.worker_health = env_or_default("HONCH_E2E_WORKER_HEALTH_URL", DEFAULT_WORKER_HEALTH_URL)
        self.clickhouse_url = env_or_default("HONCH_E2E_CLICKHOUSE_URL", DEFAULT_CLICKHOUSE_URL)
        self.project_id = env_or_default("HONCH_E2E_PROJECT_ID", DEFAULT_PROJECT_ID)
        self.database = env_or_default("HONCH_E2E_CLICKHOUSE_DATABASE", DEFAULT_CLICKHOUSE_DATABASE)

        http_get_ok(self.capture_health)
        http_get_ok(self.worker_health)
        http_get_ok(self.clickhouse_url)

    def make_client(self, queue_dir, firmware_version="e2e-fw-1"):
        return honch.Honch(
            api_key=self.token,
            endpoint_url=self.endpoint,
            device_id=None,
            device_model="MicroPython E2E",
            firmware_version=firmware_version,
            environment="e2e",
            queue_directory=queue_dir,
            batch_size=3,
            max_queued_events=100,
            max_event_bytes=8192,
            transport_timeout_ms=15000,
            disable_background_flush=True,
            battery_low_threshold=20,
        )

    def assert_common_row(self, row, event_name):
        self.assertEqual(row["event"], event_name)
        self.assertEqual(row["device_model"], "MicroPython E2E")
        self.assertEqual(row["firmware_version"], "e2e-fw-1")
        self.assertEqual(row["sdk_platform"], "micropython")
        self.assertEqual(row["environment"], "e2e")
        self.assertTrue(row["device_id"])

    def verify_event(self, event_name):
        row = wait_for_clickhouse_event(
            self.clickhouse_url,
            self.database,
            self.project_id,
            event_name,
        )
        self.assert_common_row(row, event_name)
        return row

    def test_real_capture_flow_matches_posix_e2e(self):
        with tempfile.TemporaryDirectory(prefix="honch-micropython-e2e-") as queue_dir:
            prefix = "micropython_e2e_%d_%d" % (os.getpid(), int(time.time() * 1000000) % 1000000)
            event_edge = prefix + "_edge"
            event_session = prefix + "_session"
            event_restart = prefix + "_restart"
            event_after_reset = prefix + "_after_reset"
            user_id = prefix + "_user"

            client = self.make_client(queue_dir)
            first_device_id = client.get_device_id()
            self.assertTrue(first_device_id)

            with self.assertRaises(honch.InvalidArgumentError):
                client.track("")
            with self.assertRaises(honch.InvalidArgumentError):
                client.track(event_edge, [])
            with self.assertRaises(honch.InvalidArgumentError):
                client.set_property("", "bad")

            client.track(
                event_edge,
                {
                    "source": "micropython-e2e",
                    "nested": {"mode": "hdr", "frames": [1, 2, 3]},
                    "quote": 'say "hi"',
                    "$device_id": "spoofed-device",
                    "$sdk_platform": "spoofed-platform",
                },
            )
            client.identify(user_id, {"plan": "beta", "cohort": "local"})
            client.set_property("favorite_mode", "night")
            client.session_start("recording")
            client.track(event_session, {"mode": "hdr"})
            client.session_end()
            client.flush()

            row = self.verify_event(event_edge)
            self.assertIn('"source":"micropython-e2e"', row["properties"])
            self.assertNotIn("spoofed-device", row["properties"])
            self.assertNotIn("spoofed-platform", row["properties"])
            self.assertEqual(row["device_id"], first_device_id)

            row = self.verify_event("$identify")
            self.assertEqual(row["distinct_id"], user_id)
            self.assertIn('"plan":"beta"', row["properties"])
            self.assertIn('"cohort":"local"', row["properties"])

            row = self.verify_event("$set_property")
            self.assertEqual(row["distinct_id"], user_id)
            self.assertIn('"favorite_mode":"night"', row["properties"])

            row = self.verify_event("$session_start")
            self.assertEqual(row["distinct_id"], user_id)
            self.assertTrue(row["session_id"])
            self.assertIn('"session_name":"recording"', row["properties"])

            row = self.verify_event(event_session)
            self.assertEqual(row["distinct_id"], user_id)
            self.assertTrue(row["session_id"])
            self.assertIn('"mode":"hdr"', row["properties"])

            row = self.verify_event("$session_end")
            self.assertEqual(row["distinct_id"], user_id)
            self.assertTrue(row["session_id"])

            client.shutdown()

            client = self.make_client(queue_dir)
            restarted_device_id = client.get_device_id()
            self.assertEqual(restarted_device_id, first_device_id)
            client.track(event_restart, {"phase": "restart"})
            client.flush()

            row = self.verify_event(event_restart)
            self.assertEqual(row["device_id"], first_device_id)
            self.assertEqual(row["distinct_id"], user_id)

            client.reset()
            reset_device_id = client.get_device_id()
            self.assertNotEqual(reset_device_id, first_device_id)
            client.track(event_after_reset, {"phase": "after_reset"})
            client.flush()

            row = self.verify_event(event_after_reset)
            self.assertEqual(row["device_id"], reset_device_id)
            self.assertEqual(row["distinct_id"], reset_device_id)
            self.assertNotIn("recording", row["properties"])

            client.shutdown()


if __name__ == "__main__":
    unittest.main()
