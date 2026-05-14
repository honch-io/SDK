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

try:
    import io
except ImportError:
    import uio as io

import honch

honch.__path__ = "/lib/honch"
from honch.client import Honch

API_KEY = "honch_e2e_test_key"
ENDPOINT_URL = "http://192.168.1.244:8001"
DEVICE_MODEL = "MicroPython E2E"
FIRMWARE_VERSION = "e2e-fw-1"
ENVIRONMENT = "e2e"
QUEUE_DIRECTORY = "/honch"
BATTERY_LOW_THRESHOLD = 20

# Real Pico W network credentials from the Desktop test script.
WIFI_SSID = "BT-Q2AT9P"
WIFI_PASSWORD = "eDeNEKq6PmcPLg"


class PicoWPlatform:
    def _debug(self, *parts):
        print("DEBUG:", *parts)

    def now_ms(self):
        return int(time.time() * 1000)

    def sleep_ms(self, millis):
        time.sleep(millis / 1000.0)

    def random_hex(self, nbytes=16):
        try:
            import urandom as random
        except ImportError:
            import random
        return "".join("%02x" % random.getrandbits(8) for _ in range(nbytes))

    def gzip_compress(self, data):
        self._debug("compress start", len(data), "bytes")
        try:
            import gzip

            compressed = gzip.compress(data)
            self._debug("compress gzip ok", len(compressed), "bytes")
            return compressed
        except Exception as exc:
            self._debug("compress gzip unavailable", repr(exc))

        try:
            import deflate

            buffer = io.BytesIO()
            with deflate.DeflateIO(buffer, deflate.GZIP) as stream:
                stream.write(data)
            compressed = buffer.getvalue()
            self._debug("compress deflate ok", len(compressed), "bytes")
            return compressed
        except Exception as exc:
            self._debug("compress deflate unavailable", repr(exc))
            return None

    def get_reset_reason(self):
        return "unknown"

    def get_wifi_rssi(self):
        if network is None:
            return None
        try:
            sta = network.WLAN(network.STA_IF)
            if sta.active() and sta.isconnected():
                return -64
        except Exception:
            return None
        return None

    def start_periodic(self, interval_ms, callback):
        return None

    def request_flush(self, callback):
        return False


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
    battery_level = 87
    log_state = {"battery": 0, "auto_properties": 0}

    def battery_callback():
        if log_state["battery"] == 0:
            print("DEBUG: battery callback ready")
        log_state["battery"] += 1
        return battery_level

    def auto_properties_callback():
        if log_state["auto_properties"] == 0:
            print("DEBUG: auto properties callback ready")
        log_state["auto_properties"] += 1
        return {
            "$wifi_rssi": -64,
            "adapter_property": "e2e-adapter",
            "$device_id": "adapter-spoof",
        }

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
        queue_directory=QUEUE_DIRECTORY,
        batch_size=3,
        max_queued_events=100,
        max_event_bytes=8192,
        transport_timeout_ms=15000,
        disable_background_flush=True,
        battery_callback=battery_callback,
        battery_low_threshold=BATTERY_LOW_THRESHOLD,
        auto_properties_callback=auto_properties_callback,
        platform=PicoWPlatform(),
    )

    print("DEBUG: honch device id", client.get_device_id())
    prefix = "micropython_e2e_%s" % client.platform.random_hex(4)
    event_edge = prefix + "_edge"
    event_session = prefix + "_session"
    user_id = prefix + "_user"

    battery_level = 12

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
    compression_probe = client.platform.gzip_compress(b"probe")
    if compression_probe is None:
        print(
            "DEBUG: compression unavailable on this firmware; relying on identity fallback"
        )
    else:
        print("DEBUG: compression available", len(compression_probe), "bytes")
    print(
        "DEBUG: mock batch ready",
        {
            "edge": event_edge,
            "session": event_session,
            "user": user_id,
            "battery_callback_calls": log_state["battery"],
            "auto_properties_calls": log_state["auto_properties"],
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
