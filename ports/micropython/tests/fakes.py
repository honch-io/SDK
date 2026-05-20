import gzip

from honch import cbor


class FakeClock:
    def __init__(self, millis=1_767_225_600_123):
        self._millis = millis
        self.sleeps = []

    def now_ms(self):
        return self._millis

    def advance(self, millis):
        self._millis += millis

    def sleep_ms(self, millis):
        self.sleeps.append(millis)
        self.advance(millis)


class FakeRandom:
    def __init__(self):
        self._next = 0

    def hex(self, nbytes=16):
        value = ("%032x" % self._next)[-32:]
        self._next += 1
        return value[: nbytes * 2]


class FakePlatform:
    def __init__(self, clock=None, random=None, gzip_enabled=True):
        self.clock = clock or FakeClock()
        self.random = random or FakeRandom()
        self.gzip_enabled = gzip_enabled
        self.reset_reason = "software"
        self.wifi_rssi = None
        self.periodic = None
        self.flush_requests = []

    def now_ms(self):
        return self.clock.now_ms()

    def sleep_ms(self, millis):
        self.clock.sleep_ms(millis)

    def random_hex(self, nbytes=16):
        return self.random.hex(nbytes)

    def gzip_compress(self, data):
        if not self.gzip_enabled:
            return None
        return gzip.compress(data)

    def get_reset_reason(self):
        return self.reset_reason

    def get_wifi_rssi(self):
        return self.wifi_rssi

    def start_periodic(self, interval_ms, callback):
        self.periodic = FakePeriodic(interval_ms, callback)
        return self.periodic

    def request_flush(self, callback):
        request = FakeDeferredFlush(callback)
        self.flush_requests.append(request)
        return True


class FakePeriodic:
    def __init__(self, interval_ms, callback):
        self.interval_ms = interval_ms
        self.callback = callback
        self.stopped = False

    def fire(self):
        self.callback()

    def stop(self):
        self.stopped = True


class FakeDeferredFlush:
    def __init__(self, callback):
        self.callback = callback

    def fire(self):
        self.callback()


class FakeTransport:
    def __init__(self, statuses=None):
        self.statuses = list(statuses or [202])
        self.calls = []

    def post(self, url, body, headers, timeout_ms):
        headers = dict(headers)
        encoding = headers.get("Content-Encoding")
        if encoding == "gzip":
            payload = cbor.decode(gzip.decompress(body))
        else:
            payload = cbor.decode(body)
        self.calls.append(
            {
                "url": url,
                "body": body,
                "headers": headers,
                "timeout_ms": timeout_ms,
                "payload": payload,
            }
        )
        if len(self.statuses) > 1:
            return self.statuses.pop(0)
        return self.statuses[0]
