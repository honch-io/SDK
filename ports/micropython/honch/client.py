try:
    import ujson as json
except ImportError:
    import json

try:
    import _honch_core
except ImportError:
    _honch_core = None

from .config import HonchConfig
from .errors import (
    HonchError,
    InvalidArgumentError,
    NotInitializedError,
    RateLimitedError,
    RejectedError,
    ServerError,
    StorageError,
    TransportError,
)
from .validation import require_distinct_id, require_event_name, require_json_value, require_properties


class Honch:
    def __init__(self, **kwargs):
        if _honch_core is None:
            raise ImportError("Honch MicroPython requires firmware built with the _honch_core user C module")

        if (
            kwargs.get("platform") is not None
            or kwargs.get("transport") is not None
            or kwargs.get("battery_callback") is not None
            or kwargs.get("auto_properties_callback") is not None
        ):
            raise InvalidArgumentError("Python adapter hooks are not supported by the C-core MicroPython port")

        self.config = HonchConfig(**kwargs)
        self._core = _honch_core.Client(_config_to_dict(self.config))
        self._connectivity_connected = None

    def get_device_id(self):
        return self._call("get_device_id")

    def track(self, event_name, properties=None):
        require_event_name(event_name)
        self._call("track", event_name, _json_object(require_properties(properties)))

    def identify(self, distinct_id, traits=None):
        require_distinct_id(distinct_id)
        self._call("identify", distinct_id, _json_object(require_properties(traits)))

    def set_property(self, key, value=None):
        if not isinstance(key, str) or key.strip() == "":
            raise InvalidArgumentError("invalid property key")
        require_json_value(value)
        self._call("set_property", key, _json_value(value))

    def session_start(self, session_name=None):
        if session_name is not None:
            session_name = str(session_name)
        self._call("session_start", session_name)

    def session_end(self):
        self._call("session_end")

    def connectivity_changed(self, connected):
        if connected is not True and connected is not False:
            raise InvalidArgumentError("connected must be a boolean")
        if self._connectivity_connected == connected:
            return
        self._call("connectivity_changed", connected)
        self._connectivity_connected = connected

    def connected(self):
        self.connectivity_changed(True)

    def disconnected(self):
        self.connectivity_changed(False)

    def flush(self):
        self._call("flush")

    def reset(self):
        self._call("reset")
        self._connectivity_connected = None

    def shutdown(self):
        self._call("shutdown")

    def _call(self, name, *args):
        try:
            return getattr(self._core, name)(*args)
        except Exception as exc:
            _raise_mapped(exc)


def _config_to_dict(config):
    return {
        "api_key": config.api_key,
        "endpoint_url": config.endpoint_url,
        "device_id": config.device_id,
        "device_model": config.device_model,
        "firmware_version": config.firmware_version,
        "environment": config.environment,
        "queue_directory": config.queue_directory,
        "batch_size": config.batch_size,
        "max_queued_events": config.max_queued_events,
        "max_event_bytes": config.max_event_bytes,
        "transport_timeout_ms": config.transport_timeout_ms,
        "flush_interval_seconds": config.flush_interval_seconds,
        "flush_event_threshold": config.flush_event_threshold,
        "flush_retry_initial_ms": config.flush_retry_initial_ms,
        "flush_retry_max_ms": config.flush_retry_max_ms,
        "disable_background_flush": config.disable_background_flush,
        "battery_low_threshold": config.battery_low_threshold,
    }


def _json_object(value):
    return _json_dumps(value)


def _json_value(value):
    return _json_dumps(value)


def _json_dumps(value):
    try:
        return json.dumps(value, separators=(",", ":"))
    except TypeError:
        return json.dumps(value)


def _raise_mapped(exc):
    status = getattr(exc, "status", None)
    if _honch_core is None:
        raise exc

    if status is None:
        message = str(exc)
        status = _STATUS_BY_MESSAGE.get(message)
        if status is None:
            raise exc

    if status == getattr(_honch_core, "ERROR_INVALID_ARGUMENT", None):
        raise InvalidArgumentError(str(exc))
    if status == getattr(_honch_core, "ERROR_IO", None):
        raise StorageError(str(exc))
    if status == getattr(_honch_core, "ERROR_RATE_LIMITED", None):
        raise RateLimitedError(str(exc))
    if status == getattr(_honch_core, "ERROR_SERVER", None):
        raise ServerError(str(exc))
    if status == getattr(_honch_core, "ERROR_REJECTED", None):
        raise RejectedError(str(exc))
    if status == getattr(_honch_core, "ERROR_NOT_INITIALIZED", None):
        raise NotInitializedError(str(exc))
    if status == getattr(_honch_core, "ERROR_TRANSPORT", None) or status == getattr(_honch_core, "ERROR_TIMEOUT", None):
        raise TransportError(str(exc))
    if status == getattr(_honch_core, "ERROR_QUEUE_FULL", None):
        raise StorageError(str(exc))
    raise HonchError(str(exc))


_STATUS_BY_MESSAGE = {
    "invalid argument": getattr(_honch_core, "ERROR_INVALID_ARGUMENT", None),
    "io error": getattr(_honch_core, "ERROR_IO", None),
    "transport error": getattr(_honch_core, "ERROR_TRANSPORT", None),
    "rate limited": getattr(_honch_core, "ERROR_RATE_LIMITED", None),
    "server error": getattr(_honch_core, "ERROR_SERVER", None),
    "rejected": getattr(_honch_core, "ERROR_REJECTED", None),
    "not initialized": getattr(_honch_core, "ERROR_NOT_INITIALIZED", None),
    "queue full": getattr(_honch_core, "ERROR_QUEUE_FULL", None),
    "timeout": getattr(_honch_core, "ERROR_TIMEOUT", None),
} if _honch_core is not None else {}
