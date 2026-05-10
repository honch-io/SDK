import honch


class BoardPlatform:
    def now_ms(self):
        import time
        return int(time.time() * 1000)

    def sleep_ms(self, millis):
        import time
        time.sleep(millis / 1000)

    def random_hex(self, nbytes=16):
        import random
        return "".join("%02x" % random.getrandbits(8) for _ in range(nbytes))

    def gzip_compress(self, data):
        return None

    def get_reset_reason(self):
        return "unknown"

    def get_wifi_rssi(self):
        return None


def battery_level():
    return -1


client = honch.Honch(
    api_key="your-api-key",
    endpoint_url="https://capture.honch.io",
    device_model="custom-board",
    firmware_version="1.0.0",
    queue_directory="/honch",
    platform=BoardPlatform(),
    battery_callback=battery_level,
)

client.track("custom_platform_ready")
