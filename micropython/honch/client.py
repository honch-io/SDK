from .config import HonchConfig
from .encoder import build_batch_from_events, build_event, decode_event, encode_event
from .errors import InvalidArgumentError, RejectedError
from .identity import IdentityStore
from .platform import DefaultPlatform
from .queue import PersistentQueue
from .scheduler import Scheduler
from .transport import HttpTransport, post_batch
from .validation import require_distinct_id, require_event_name, require_json_value, require_properties


class Honch:
    def __init__(self, **kwargs):
        self.config = HonchConfig(**kwargs)
        self.platform = kwargs.get("platform") or DefaultPlatform()
        self.transport = kwargs.get("transport") or HttpTransport()
        self.queue = PersistentQueue(
            self.config.queue_directory,
            self.platform,
            self.config.max_queued_events,
            self.config.max_event_bytes,
        )
        self.identity = IdentityStore(self.queue.state_dir, self.platform, self.config.device_id)
        self.session_id = None
        self.battery_low_emitted = False
        self._connectivity_connected = None
        self.scheduler = Scheduler(self)

        changed, previous = self.identity.check_firmware_version(self.config.firmware_version)
        if changed:
            self._track_internal(
                "$firmware_update",
                {"previous_version": previous, "new_version": self.config.firmware_version},
                check_battery_low=True,
            )
        self._track_internal("$device_boot", {"reset_reason": self.platform.get_reset_reason()}, check_battery_low=True)

    def get_device_id(self):
        return self.identity.device_id

    def track(self, event_name, properties=None):
        require_event_name(event_name)
        props = require_properties(properties)
        self._track_internal(event_name, props, check_battery_low=True)

    def identify(self, distinct_id, traits=None):
        require_distinct_id(distinct_id)
        props = require_properties(traits)
        previous = self.identity.set_distinct_id(distinct_id)
        try:
            self._track_internal("$identify", props, check_battery_low=False)
        except Exception:
            self.identity.restore_distinct_id(previous)
            raise

    def set_property(self, key, value=None):
        if not isinstance(key, str) or key.strip() == "":
            raise InvalidArgumentError("invalid property key")
        require_json_value(value)
        self.track("$set_property", {key: value})

    def session_start(self, session_name=None):
        if self.session_id is not None:
            self._track_internal("$session_end", {}, check_battery_low=True)
            self.session_id = None
        self.session_id = "sess_" + self.platform.random_hex(16)
        props = {}
        if session_name is not None and str(session_name).strip() != "":
            props["session_name"] = str(session_name)
        try:
            self._track_internal("$session_start", props, check_battery_low=True)
        except Exception:
            self.session_id = None
            raise

    def session_end(self):
        if self.session_id is None:
            return
        self._track_internal("$session_end", {}, check_battery_low=True)
        self.session_id = None

    def connectivity_changed(self, connected):
        if connected is not True and connected is not False:
            raise InvalidArgumentError("connected must be a boolean")
        if self._connectivity_connected == connected:
            return
        state = "connected" if connected else "disconnected"
        self._track_internal("$connectivity_change", {"state": state}, check_battery_low=True)
        self._connectivity_connected = connected
        if connected:
            self.scheduler.request_flush()

    def connected(self):
        self.connectivity_changed(True)

    def disconnected(self):
        self.connectivity_changed(False)

    def flush(self):
        final_error = None
        while self.queue.count() > 0:
            batch = self.queue.read_batch(self.config.batch_size)
            names = []
            events = []
            invalid_names = []
            for name, text in batch:
                if len(text) > self.config.max_event_bytes:
                    invalid_names.append(name)
                    continue
                try:
                    events.append(decode_event(text))
                except (TypeError, ValueError):
                    invalid_names.append(name)
                    continue
                names.append(name)
            if invalid_names:
                self.queue.move_to_dead(invalid_names)
                final_error = RejectedError("invalid persisted event")
            if not events:
                continue
            payload = build_batch_from_events(self.config.api_key, events)
            try:
                post_batch(self.config, self.platform, self.transport, payload)
            except RejectedError as exc:
                self.queue.move_to_dead(names)
                raise exc
            except Exception:
                raise
            else:
                self.queue.delete_pending(names)
        if final_error is not None:
            raise final_error

    def reset(self):
        self.identity.reset()
        self.session_id = None
        self.battery_low_emitted = False
        self.queue.clear()

    def shutdown(self):
        self.scheduler.stop()
        self._track_internal("$device_shutdown", {}, check_battery_low=True, notify_scheduler=False)
        self.flush()

    def _track_internal(self, event_name, properties, check_battery_low, notify_scheduler=True):
        battery_level = self._read_battery_level()
        event = build_event(self, event_name, properties, battery_level)
        self.queue.enqueue(encode_event(event))
        if notify_scheduler:
            self.scheduler.after_enqueue()
        if check_battery_low:
            self._emit_battery_low_if_needed(battery_level)

    def _read_battery_level(self):
        callback = self.config.battery_callback
        if callback is None:
            return None
        try:
            value = callback()
        except Exception:
            return None
        if value is None or value < 0:
            return None
        return int(value)

    def _emit_battery_low_if_needed(self, battery_level):
        if battery_level is None:
            return
        if battery_level >= self.config.battery_low_threshold:
            self.battery_low_emitted = False
            return
        if self.battery_low_emitted:
            return
        self.battery_low_emitted = True
        self._track_internal(
            "$battery_low",
            {"level": battery_level},
            check_battery_low=False,
            notify_scheduler=True,
        )
