from .errors import InvalidArgumentError


SDK_VERSION = "0.2.0"
SDK_PLATFORM = "micropython"

DEFAULT_BATCH_SIZE = 20
MAX_BATCH_SIZE = 50
DEFAULT_MAX_QUEUED_EVENTS = 1000
DEFAULT_MAX_EVENT_BYTES = 16384
DEFAULT_TRANSPORT_TIMEOUT_MS = 10000
DEFAULT_FLUSH_INTERVAL_SECONDS = 60
DEFAULT_FLUSH_EVENT_THRESHOLD = 30
DEFAULT_FLUSH_RETRY_INITIAL_MS = 1000
DEFAULT_FLUSH_RETRY_MAX_MS = 300000
DEFAULT_BATTERY_LOW_THRESHOLD = 15
DEFAULT_GZIP_MIN_BYTES = 1024


def is_blank(value):
    return value is None or str(value).strip() == ""


class HonchConfig:
    def __init__(self, **kwargs):
        required = ("api_key", "endpoint_url", "device_model", "firmware_version", "queue_directory")
        for key in required:
            if is_blank(kwargs.get(key)):
                raise InvalidArgumentError("missing required config: " + key)

        self.api_key = str(kwargs["api_key"])
        self.endpoint_url = str(kwargs["endpoint_url"])
        self.device_id = kwargs.get("device_id")
        self.device_model = str(kwargs["device_model"])
        self.firmware_version = str(kwargs["firmware_version"])
        self.environment = str(kwargs.get("environment") or "production")
        self.queue_directory = str(kwargs["queue_directory"])
        self.batch_size = int(kwargs.get("batch_size") or DEFAULT_BATCH_SIZE)
        if self.batch_size > MAX_BATCH_SIZE:
            self.batch_size = MAX_BATCH_SIZE
        if self.batch_size <= 0:
            self.batch_size = DEFAULT_BATCH_SIZE

        self.max_queued_events = int(kwargs.get("max_queued_events") or DEFAULT_MAX_QUEUED_EVENTS)
        self.max_event_bytes = int(kwargs.get("max_event_bytes") or DEFAULT_MAX_EVENT_BYTES)
        self.transport_timeout_ms = int(kwargs.get("transport_timeout_ms") or DEFAULT_TRANSPORT_TIMEOUT_MS)
        self.flush_interval_seconds = int(kwargs.get("flush_interval_seconds") or DEFAULT_FLUSH_INTERVAL_SECONDS)
        self.flush_event_threshold = int(kwargs.get("flush_event_threshold") or DEFAULT_FLUSH_EVENT_THRESHOLD)
        self.flush_retry_initial_ms = int(kwargs.get("flush_retry_initial_ms") or DEFAULT_FLUSH_RETRY_INITIAL_MS)
        self.flush_retry_max_ms = int(kwargs.get("flush_retry_max_ms") or DEFAULT_FLUSH_RETRY_MAX_MS)
        if self.flush_retry_max_ms < self.flush_retry_initial_ms:
            self.flush_retry_max_ms = self.flush_retry_initial_ms
        self.disable_gzip = bool(kwargs.get("disable_gzip", False))
        self.gzip_min_bytes = int(kwargs.get("gzip_min_bytes") or DEFAULT_GZIP_MIN_BYTES)
        self.disable_background_flush = bool(kwargs.get("disable_background_flush", False))
        self.battery_callback = kwargs.get("battery_callback")
        self.battery_low_threshold = int(kwargs.get("battery_low_threshold") or DEFAULT_BATTERY_LOW_THRESHOLD)
        self.auto_properties_callback = kwargs.get("auto_properties_callback")
