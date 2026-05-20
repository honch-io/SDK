from .errors import InvalidArgumentError


MAX_EVENT_NAME = 128
MAX_DISTINCT_ID = 256

RESERVED_PROPERTIES = {
    "$battery_level",
    "$device_id",
    "$device_model",
    "$environment",
    "$firmware_version",
    "$sdk_platform",
    "$sdk_version",
    "$session_id",
    "$wifi_rssi",
}


def require_event_name(event):
    if not isinstance(event, str) or event.strip() == "" or len(event) > MAX_EVENT_NAME:
        raise InvalidArgumentError("invalid event name")


def require_distinct_id(distinct_id):
    if not isinstance(distinct_id, str) or distinct_id.strip() == "" or len(distinct_id) > MAX_DISTINCT_ID:
        raise InvalidArgumentError("invalid distinct_id")


def require_properties(properties):
    if properties is None:
        return {}
    if not isinstance(properties, dict):
        raise InvalidArgumentError("properties must be a dict")
    _validate_json_value(properties)
    return dict(properties)


def require_json_value(value):
    _validate_json_value(value)
    return value


def _validate_json_value(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return
    if isinstance(value, list):
        for item in value:
            _validate_json_value(item)
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise InvalidArgumentError("property keys must be strings")
            _validate_json_value(item)
        return
    raise InvalidArgumentError("unsupported JSON value")
