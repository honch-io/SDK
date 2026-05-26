try:
    import os
except ImportError:
    os = None

import honch


def getenv(name, fallback):
    if os is None or not hasattr(os, "getenv"):
        return fallback
    value = os.getenv(name)
    return value or fallback


queue_directory = getenv("HONCH_MICROPYTHON_QUEUE_DIR", "/tmp/honch-micropython-runtime-smoke")

client = honch.Honch(
    api_key="runtime-smoke-key",
    endpoint_url="http://127.0.0.1:9",
    device_model="MicroPython Unix",
    firmware_version="runtime-smoke",
    environment="test",
    queue_directory=queue_directory,
    batch_size=2,
    max_queued_events=8,
    max_event_bytes=4096,
    disable_background_flush=True,
)

device_id = client.get_device_id()
assert device_id is not None
assert len(device_id) > 0

client.track("runtime_smoke", {"source": "micropython-unix"})
client.identify("runtime-user", {"plan": "test"})
client.set_property("runtime", True)
client.session_start("runtime")
client.session_end()
client.shutdown()
