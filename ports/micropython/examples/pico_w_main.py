try:
    import network
except ImportError:
    network = None

try:
    import time
except ImportError:
    import utime as time

try:
    import sys
except ImportError:
    sys = None

import honch

honch.__path__ = "/lib/honch"
from honch.client import Honch

API_KEY = "honch_e2e_test_key"
ENDPOINT_URL = "http://192.168.1.244:8001"
DEVICE_MODEL = "MicroPython E2E"
FIRMWARE_VERSION = "e2e-fw-1"
ENVIRONMENT = "e2e"
BATTERY_LOW_THRESHOLD = 20

# Real Pico W network credentials from the Desktop test script.
WIFI_SSID = "BT-Q2AT9P"
WIFI_PASSWORD = "eDeNEKq6PmcPLg"


def connect_wifi(ssid, password, timeout_s=20):
    if network is None:
        raise RuntimeError("network module is unavailable")

    print("DEBUG: wifi init")
    sta = network.WLAN(network.STA_IF)
    if not sta.active():
        print("DEBUG: wifi activating STA")
        sta.active(True)
    if sta.isconnected():
        print("DEBUG: wifi already connected", sta.ifconfig())
        return sta

    print("DEBUG: wifi connecting to", ssid)
    sta.connect(ssid, password)
    deadline = time.time() + timeout_s
    while not sta.isconnected():
        if time.time() >= deadline:
            raise RuntimeError("Wi-Fi connection timed out")
        time.sleep_ms(250)
        print("DEBUG: wifi waiting", sta.status())
    print("DEBUG: wifi connected", sta.ifconfig())
    return sta


def main():
    print("DEBUG: boot start")
    try:
        connect_wifi(WIFI_SSID, WIFI_PASSWORD)
    except Exception as exc:
        print("DEBUG: wifi failed", repr(exc))
        if sys is not None and hasattr(sys, "print_exception"):
            sys.print_exception(exc)
        raise

    print("DEBUG: honch init")

    client = Honch(
        api_key=API_KEY,
        endpoint_url=ENDPOINT_URL,
        device_id=None,
        device_model=DEVICE_MODEL,
        firmware_version=FIRMWARE_VERSION,
        environment=ENVIRONMENT,
        event_buffer=bytearray(16384),
        batch_size=3,
        max_queued_events=100,
        max_event_bytes=8192,
        transport_timeout_ms=15000,
        battery_low_threshold=BATTERY_LOW_THRESHOLD,
    )

    print("DEBUG: honch device id", client.get_device_id())
    prefix = "micropython_e2e_%d" % int(time.time())
    event_edge = prefix + "_edge"
    event_session = prefix + "_session"
    user_id = prefix + "_user"

    print("DEBUG: track edge", event_edge)
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
    print("DEBUG: identify", user_id)
    client.identify(user_id, {"plan": "beta", "cohort": "local"})
    print("DEBUG: set property favorite_mode")
    client.set_property("favorite_mode", "night")
    print("DEBUG: session start")
    client.session_start("recording")
    print("DEBUG: track session", event_session)
    client.track(event_session, {"mode": "hdr"})
    print("DEBUG: session end")
    client.session_end()
    print(
        "DEBUG: mock batch ready",
        {
            "edge": event_edge,
            "session": event_session,
            "user": user_id,
        },
    )
    print("DEBUG: flush start")
    try:
        client.flush()
        print("DEBUG: flush ok")
    except Exception as exc:
        print("DEBUG: flush failed", repr(exc))
        if sys is not None and hasattr(sys, "print_exception"):
            sys.print_exception(exc)
        raise


main()
