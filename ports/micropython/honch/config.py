from .errors import InvalidArgumentError


SDK_VERSION = "0.3.0"
SDK_PLATFORM = "micropython"
DEFAULT_ENDPOINT_URL = "https://i.honch.io"

DEFAULT_BATCH_SIZE = 20
MAX_BATCH_SIZE = 50
DEFAULT_MAX_QUEUED_EVENTS = 1000
DEFAULT_MAX_EVENT_BYTES = 8192
DEFAULT_TRANSPORT_TIMEOUT_MS = 8000
MAX_TRANSPORT_TIMEOUT_MS = 10000
DEFAULT_FLUSH_INTERVAL_SECONDS = 120
DEFAULT_FLUSH_MIN_INTERVAL_MS = 15000
FLUSH_MIN_INTERVAL_DISABLED_MS = 0xFFFFFFFF
DEFAULT_FLUSH_EVENT_THRESHOLD = 20
DEFAULT_FLUSH_RETRY_INITIAL_MS = 1000
DEFAULT_FLUSH_RETRY_MAX_MS = 300000
DEFAULT_BATTERY_LOW_THRESHOLD = 15


def is_blank(value):
    return value is None or str(value).strip() == ""


class HonchConfig:
    def __init__(self, **kwargs):
        required = ("api_key", "device_id", "device_model", "firmware_version", "event_buffer")
        for key in required:
            if is_blank(kwargs.get(key)):
                raise InvalidArgumentError("missing required config: " + key)

        self.api_key = str(kwargs["api_key"])
        endpoint_url = kwargs.get("endpoint_url")
        self.endpoint_url = DEFAULT_ENDPOINT_URL if is_blank(endpoint_url) else str(endpoint_url)
        self.device_id = kwargs.get("device_id")
        self.device_model = str(kwargs["device_model"])
        self.firmware_version = str(kwargs["firmware_version"])
        self.environment = str(kwargs.get("environment") or "production")
        self.event_buffer = kwargs["event_buffer"]
        self.batch_size = int(kwargs.get("batch_size") or DEFAULT_BATCH_SIZE)
        if self.batch_size > MAX_BATCH_SIZE:
            self.batch_size = MAX_BATCH_SIZE
        if self.batch_size <= 0:
            self.batch_size = DEFAULT_BATCH_SIZE

        max_queued_events = kwargs.get("max_queued_events")
        self.max_queued_events = DEFAULT_MAX_QUEUED_EVENTS if max_queued_events is None else int(max_queued_events)
        if self.max_queued_events <= 0:
            raise InvalidArgumentError("max_queued_events must be positive")

        max_event_bytes = kwargs.get("max_event_bytes")
        self.max_event_bytes = DEFAULT_MAX_EVENT_BYTES if max_event_bytes is None else int(max_event_bytes)
        if self.max_event_bytes <= 0:
            raise InvalidArgumentError("max_event_bytes must be positive")

        timeout = kwargs.get("transport_timeout_ms")
        self.transport_timeout_ms = DEFAULT_TRANSPORT_TIMEOUT_MS if timeout is None else int(timeout)
        if self.transport_timeout_ms <= 0:
            raise InvalidArgumentError("transport_timeout_ms must be positive")
        if self.transport_timeout_ms > MAX_TRANSPORT_TIMEOUT_MS:
            self.transport_timeout_ms = MAX_TRANSPORT_TIMEOUT_MS
        self.flush_interval_seconds = int(kwargs.get("flush_interval_seconds") or DEFAULT_FLUSH_INTERVAL_SECONDS)
        self.flush_min_interval_ms = int(kwargs.get("flush_min_interval_ms") or DEFAULT_FLUSH_MIN_INTERVAL_MS)
        self.flush_event_threshold = int(kwargs.get("flush_event_threshold") or DEFAULT_FLUSH_EVENT_THRESHOLD)
        self.flush_retry_initial_ms = int(kwargs.get("flush_retry_initial_ms") or DEFAULT_FLUSH_RETRY_INITIAL_MS)
        self.flush_retry_max_ms = int(kwargs.get("flush_retry_max_ms") or DEFAULT_FLUSH_RETRY_MAX_MS)
        if self.flush_retry_max_ms < self.flush_retry_initial_ms:
            self.flush_retry_max_ms = self.flush_retry_initial_ms
        self.battery_callback = kwargs.get("battery_callback")
        self.battery_low_threshold = int(kwargs.get("battery_low_threshold") or DEFAULT_BATTERY_LOW_THRESHOLD)
        self.auto_properties_callback = kwargs.get("auto_properties_callback")
        self.connectivity_callback = kwargs.get("connectivity_callback")
