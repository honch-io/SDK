try:
    import utime as time
except ImportError:
    import time

try:
    import os
except ImportError:
    os = None


class DefaultPlatform:
    def now_ms(self):
        if hasattr(time, "time_ns"):
            return time.time_ns() // 1000000
        if hasattr(time, "ticks_ms"):
            return time.ticks_ms()
        return int(time.time() * 1000)

    def sleep_ms(self, millis):
        if hasattr(time, "sleep_ms"):
            time.sleep_ms(millis)
        else:
            time.sleep(millis / 1000.0)

    def random_hex(self, nbytes=16):
        try:
            import urandom as random
        except ImportError:
            import random

        chars = []
        for _ in range(nbytes):
            chars.append("%02x" % random.getrandbits(8))
        return "".join(chars)

    def gzip_compress(self, data):
        try:
            import gzip
            return gzip.compress(data)
        except (ImportError, AttributeError):
            return None

    def get_reset_reason(self):
        return "unknown"

    def get_wifi_rssi(self):
        return None

    def start_periodic(self, interval_ms, callback):
        return None

    def request_flush(self, callback):
        return False


def ensure_dir(path):
    parts = []
    current = path
    while current and current not in ("/", "."):
        parts.append(current)
        parent = _dirname(current)
        if parent == current:
            break
        current = parent
    for directory in reversed(parts):
        try:
            os.mkdir(directory)
        except OSError:
            pass


def list_files(path, suffix):
    try:
        names = os.listdir(path)
    except OSError:
        return []
    return sorted(name for name in names if name.endswith(suffix))


def remove_if_exists(path):
    try:
        os.remove(path)
    except OSError:
        pass


def write_text_atomic(directory, filename, content):
    ensure_dir(directory)
    tmp = join_path(directory, filename + ".tmp")
    final = join_path(directory, filename)
    with open(tmp, "w") as fh:
        fh.write(content)
    try:
        os.rename(tmp, final)
    except OSError:
        remove_if_exists(final)
        os.rename(tmp, final)


def read_text(path):
    with open(path, "r") as fh:
        return fh.read().strip()


def write_text(directory, filename, content):
    write_text_atomic(directory, filename, content)


def write_bytes_atomic(directory, filename, content):
    ensure_dir(directory)
    tmp = join_path(directory, filename + ".tmp")
    final = join_path(directory, filename)
    with open(tmp, "wb") as fh:
        fh.write(content)
    try:
        os.rename(tmp, final)
    except OSError:
        remove_if_exists(final)
        os.rename(tmp, final)


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def join_path(left, right):
    if left.endswith("/"):
        return left + right
    return left + "/" + right


def _dirname(path):
    stripped = path.rstrip("/")
    index = stripped.rfind("/")
    if index <= 0:
        return "/" if stripped.startswith("/") else "."
    return stripped[:index]
