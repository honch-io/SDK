import os
import time
import urllib.parse
import urllib.request


DEFAULT_PROJECT_ID = "00000000-0000-0000-0000-000000000002"
DEFAULT_CLICKHOUSE_URL = "http://127.0.0.1:8123"
DEFAULT_CLICKHOUSE_DATABASE = "platform"


def env_or_default(name, default):
    value = os.environ.get(name)
    return value if value else default


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
    return dict(zip(names, fields))


def fetch_event(clickhouse_url, database, project_id, event_name, distinct_id=None):
    escaped_event = event_name.replace("'", "\\'")
    distinct_filter = ""
    if distinct_id is not None:
        escaped_distinct_id = distinct_id.replace("'", "\\'")
        distinct_filter = " AND distinct_id = '%s'" % escaped_distinct_id
    query = (
        "SELECT event, distinct_id, properties, ifNull(device_id, ''), "
        "ifNull(device_model, ''), ifNull(firmware_version, ''), "
        "ifNull(session_id, ''), ifNull(sdk_platform, ''), ifNull(environment, '') "
        "FROM %s.events WHERE team_id = '%s' AND event = '%s'%s "
        "ORDER BY received_at DESC LIMIT 1 FORMAT TabSeparated"
        % (database, project_id, escaped_event, distinct_filter)
    )
    body = clickhouse_query(clickhouse_url, query)
    return parse_row(body) if body else None


def wait_for_event(clickhouse_url, database, project_id, event_name, distinct_id=None):
    for _ in range(45):
        row = fetch_event(clickhouse_url, database, project_id, event_name, distinct_id)
        if row is not None and row.get("event"):
            return row
        time.sleep(1)
    raise AssertionError("E2E event did not appear in ClickHouse: %s" % event_name)


def assert_common_row(row, event_name):
    assert row["event"] == event_name, row
    assert row["device_model"] == "MicroPython E2E", row
    assert row["firmware_version"] == "e2e-fw-1", row
    assert row["sdk_platform"] == "micropython", row
    assert row["environment"] == "e2e", row
    assert row["device_id"], row


def main():
    prefix = os.environ["HONCH_MICROPYTHON_E2E_PREFIX"]
    clickhouse_url = env_or_default("HONCH_E2E_CLICKHOUSE_URL", DEFAULT_CLICKHOUSE_URL)
    project_id = env_or_default("HONCH_E2E_PROJECT_ID", DEFAULT_PROJECT_ID)
    database = env_or_default("HONCH_E2E_CLICKHOUSE_DATABASE", DEFAULT_CLICKHOUSE_DATABASE)

    event_edge = prefix + "_edge"
    user_id = prefix + "_user"

    row = wait_for_event(clickhouse_url, database, project_id, event_edge)
    assert_common_row(row, event_edge)
    assert '"source":"micropython-e2e"' in row["properties"], row
    assert '"quote":' in row["properties"], row
    assert "say" in row["properties"] and "hi" in row["properties"], row

    row = wait_for_event(clickhouse_url, database, project_id, "$identify", user_id)
    assert row["distinct_id"] == user_id, row
    assert '"plan":"beta"' in row["properties"], row
    assert '"cohort":"local"' in row["properties"], row

    row = wait_for_event(clickhouse_url, database, project_id, "$set_property", user_id)
    assert row["distinct_id"] == user_id, row
    assert '"favorite_mode":"night"' in row["properties"], row


if __name__ == "__main__":
    main()
