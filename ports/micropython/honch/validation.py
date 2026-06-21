from .errors import InvalidArgumentError


MAX_EVENT_NAME = 128
MAX_DISTINCT_ID = 256


def require_text(value, label, max_len=None):
    # Single source of truth for the layer's "non-blank string, optional max
    # length" rule. Core is still the final gate (and counts UTF-8 bytes, not
    # code points), so these checks exist for early, well-attributed errors.
    if not isinstance(value, str) or value.strip() == "" or (max_len is not None and len(value) > max_len):
        raise InvalidArgumentError("invalid " + label)


def require_event_name(event):
    require_text(event, "event name", MAX_EVENT_NAME)


def require_distinct_id(distinct_id):
    require_text(distinct_id, "distinct_id", MAX_DISTINCT_ID)


def require_properties(properties):
    if properties is None:
        return {}
    if not isinstance(properties, dict):
        raise InvalidArgumentError("properties must be a dict")
    _validate_typed_value(properties)
    return dict(properties)


def require_value(value):
    _validate_typed_value(value)
    return value


def _validate_typed_value(value):
    if value is None or isinstance(value, (bool, int, float, str, bytes)):
        return
    if isinstance(value, (list, tuple)):
        for item in value:
            _validate_typed_value(item)
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise InvalidArgumentError("property keys must be strings")
            _validate_typed_value(item)
        return
    raise InvalidArgumentError("unsupported property value")
