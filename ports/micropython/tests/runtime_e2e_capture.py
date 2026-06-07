import os
import time

import honch


def env_or_default(name, default):
    value = os.getenv(name)
    return value if value else default


prefix = env_or_default("HONCH_MICROPYTHON_E2E_PREFIX", "micropython_e2e_%d" % int(time.time()))
event_edge = prefix + "_edge"
user_id = prefix + "_user"

client = honch.Honch(
    api_key=env_or_default("HONCH_E2E_TOKEN", "honch_e2e_test_key"),
    endpoint_url=env_or_default("HONCH_E2E_ENDPOINT", "http://127.0.0.1:8001"),
    device_id=prefix + "_device",
    device_model="MicroPython E2E",
    firmware_version="e2e-fw-1",
    environment="e2e",
    event_buffer=bytearray(8192),
    batch_size=8,
    max_queued_events=8,
    max_event_bytes=4096,
    transport_timeout_ms=10000,
    battery_low_threshold=20,
)

first_device_id = client.get_device_id()
assert first_device_id

client.track(
    event_edge,
    {
        "source": "micropython-e2e",
        "quote": 'say "hi"',
    },
)
client.identify(user_id, {"plan": "beta", "cohort": "local"})
client.set_property("favorite_mode", "night")
client.flush()
client.shutdown()

print("HONCH_MICROPYTHON_E2E_PREFIX=%s" % prefix)
print("HONCH_MICROPYTHON_E2E_DEVICE_ID=%s" % first_device_id)
