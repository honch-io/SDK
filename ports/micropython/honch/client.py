try:
    import _honch_core
except ImportError:
    _honch_core = None

from .config import HonchConfig
from .errors import (
    HonchError,
    InvalidArgumentError,
    NotInitializedError,
    OfflineError,
    RateLimitedError,
    RejectedError,
    ServerError,
    StorageError,
    TransportError,
)
from .validation import (
    require_distinct_id,
    require_event_name,
    require_properties,
    require_text,
    require_value,
)

# Cap how much of the Python stack we format before the C core's own bound. The
# $crash `backtrace` property is small (the core keeps the FIRST bytes and drops
# the rest), so we format CRASH-SITE-FIRST below — truncation then trims the
# outermost frames, not the ones that located the bug.
_CRASH_BACKTRACE_MAX_FRAMES = 10


def _format_crash_backtrace(exc):
    """Format an exception's traceback compact and crash-site-first, e.g.
    `sensor.py:42 in read <- app.py:10 in loop <- main.py:88 in <module>`.

    On MicroPython the readable backtrace is the Python traceback (already
    file/line/function-resolved) — there is no coredump to symbolicate — so this
    is what gives a MicroPython `$crash` the "where", not just the "what".

    Returns the formatted string, or None if it can't be produced. MUST NOT raise:
    it runs inside the excepthook, where a failure must never displace delivery of
    the original exception."""
    try:
        import sys
        import io

        buf = io.StringIO()
        sys.print_exception(exc, buf)  # MicroPython: prints most-recent-call-LAST
        frames = []
        for raw in buf.getvalue().split("\n"):
            line = raw.strip()
            # "File "main.py", line 42, in read"
            if not line.startswith("File ") or ", line " not in line:
                continue
            try:
                fname = line.split('"', 2)[1]
                rest = line.split(", line ", 1)[1]
                lineno = rest.split(",", 1)[0].strip()
                func = rest.split(" in ", 1)[1].strip() if " in " in rest else "?"
                frames.append("%s:%s in %s" % (fname, lineno, func))
            except Exception:
                continue
        if not frames:
            return None
        frames.reverse()  # crash site first, so it survives a start-truncation
        return " <- ".join(frames[:_CRASH_BACKTRACE_MAX_FRAMES])
    except Exception:
        return None


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
        self._honch_excepthook = None
        self._previous_excepthook = None

    def get_device_id(self):
        return self._call("get_device_id")

    def queue_stats(self):
        return self._call("queue_stats")

    def track(self, event_name, properties=None):
        require_event_name(event_name)
        self._call("track", event_name, require_properties(properties))

    def report_log_error(self, message, *, component=None):
        """Capture a logged error as a bounded, coalesced $error event.

        Intended to be driven automatically (e.g. from a logging.Handler), not
        sprinkled through application code.
        """
        require_text(message, "error message")
        self._call("report_log_error", component, message)

    def install_error_hook(self):
        try:
            import sys
        except ImportError:
            return False
        # Installing twice would stack our wrapper on top of itself and report
        # every exception once per layer, so a repeat call is a no-op.
        if self._honch_excepthook is not None:
            return True
        previous_hook = getattr(sys, "excepthook", None)
        if previous_hook is None:
            return False

        def honch_excepthook(exc_type, exc, tb):
            try:
                report = {
                    "message": str(exc) or repr(exc),
                    "type": getattr(exc_type, "__name__", "Exception"),
                }
                backtrace = _format_crash_backtrace(exc)
                if backtrace:
                    report["backtrace"] = backtrace
                self._call("report_crash", report)
            except Exception:
                # An excepthook must never raise: a reporting failure must not
                # replace delivery of the original exception to the next hook.
                pass
            finally:
                previous_hook(exc_type, exc, tb)

        self._previous_excepthook = previous_hook
        self._honch_excepthook = honch_excepthook
        sys.excepthook = honch_excepthook
        return True

    def uninstall_error_hook(self):
        try:
            import sys
        except ImportError:
            return False
        if self._honch_excepthook is None:
            return False
        # Only restore if our hook is still the active one; if something was
        # installed on top of us we cannot safely splice ourselves out.
        if getattr(sys, "excepthook", None) is self._honch_excepthook:
            sys.excepthook = self._previous_excepthook
        self._honch_excepthook = None
        self._previous_excepthook = None
        return True

    def identify(self, distinct_id, traits=None):
        require_distinct_id(distinct_id)
        self._call("identify", distinct_id, require_properties(traits))

    def set_property(self, key, value=None):
        require_text(key, "property key")
        self._call("set_property", key, require_value(value))

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
        # The C core is never given the Python connectivity *poll* callback (only
        # connectivity_changed events are forwarded), so it cannot gate on it
        # itself. Uphold the cross-port flush contract here: flushing while the
        # callback reports offline raises OfflineError before core is consulted.
        if not self._connectivity_available():
            raise OfflineError("offline")
        self._call("flush")

    def tick(self):
        if not self._connectivity_available():
            return
        self._call("tick")

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

    def _connectivity_available(self):
        callback = self.config.connectivity_callback
        if callback is None:
            return True
        return bool(callback())


def _config_to_dict(config):
    return {
        "api_key": config.api_key,
        "endpoint_url": config.endpoint_url,
        "device_id": config.device_id,
        "device_model": config.device_model,
        "firmware_version": config.firmware_version,
        "environment": config.environment,
        "event_buffer": config.event_buffer,
        "batch_size": config.batch_size,
        "max_queued_events": config.max_queued_events,
        "max_event_bytes": config.max_event_bytes,
        "transport_timeout_ms": config.transport_timeout_ms,
        "flush_interval_seconds": config.flush_interval_seconds,
        "flush_min_interval_ms": config.flush_min_interval_ms,
        "flush_event_threshold": config.flush_event_threshold,
        "flush_retry_initial_ms": config.flush_retry_initial_ms,
        "flush_retry_max_ms": config.flush_retry_max_ms,
        "battery_low_threshold": config.battery_low_threshold,
    }


def _raise_mapped(exc):
    status = getattr(exc, "status", None)
    if _honch_core is None:
        raise exc

    if status is None:
        message = str(exc)
        status = _STATUS_BY_MESSAGE.get(message)
        if status is None:
            # Unknown/unmapped status string (wording drift, or a status with no
            # table entry): wrap as HonchError so callers can always catch it,
            # rather than leaking a bare RuntimeError from the C module.
            raise HonchError(str(exc))

    if status == getattr(_honch_core, "ERROR_INVALID_ARGUMENT", None):
        raise InvalidArgumentError(str(exc))
    if status == getattr(_honch_core, "ERROR_IO", None):
        raise StorageError(str(exc))
    if status == getattr(_honch_core, "ERROR_RATE_LIMITED", None):
        raise RateLimitedError(str(exc))
    if status == getattr(_honch_core, "ERROR_OFFLINE", None):
        raise OfflineError(str(exc))
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
    if status == getattr(_honch_core, "ERROR_BUSY", None):
        raise HonchError(str(exc))
    if status == getattr(_honch_core, "ERROR_NOT_SUPPORTED", None):
        raise HonchError(str(exc))
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
    "busy": getattr(_honch_core, "ERROR_BUSY", None),
    "not supported": getattr(_honch_core, "ERROR_NOT_SUPPORTED", None),
    "offline": getattr(_honch_core, "ERROR_OFFLINE", None),
    "out of memory": getattr(_honch_core, "ERROR_OUT_OF_MEMORY", None),
    "already initialized": getattr(_honch_core, "ERROR_ALREADY_INITIALIZED", None),
    "internal error": getattr(_honch_core, "ERROR_INTERNAL", None),
} if _honch_core is not None else {}
