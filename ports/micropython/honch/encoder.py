from . import cbor
from .config import SDK_PLATFORM, SDK_VERSION
from .validation import RESERVED_PROPERTIES, require_properties


def build_event(client, event_name, properties, battery_level=None):
    merged = {}
    for key, value in properties.items():
        if key not in RESERVED_PROPERTIES:
            merged[key] = value

    adapter = _adapter_properties(client)
    for key, value in adapter.items():
        if key == "$wifi_rssi" or key not in RESERVED_PROPERTIES:
            merged[key] = value

    wifi_rssi = client.platform.get_wifi_rssi()
    if wifi_rssi is not None:
        merged["$wifi_rssi"] = wifi_rssi

    merged["$device_id"] = client.identity.device_id
    if client.session_id is not None:
        merged["$session_id"] = client.session_id
    merged["$device_model"] = client.config.device_model
    merged["$firmware_version"] = client.config.firmware_version
    merged["$environment"] = client.config.environment
    if battery_level is not None and 0 <= battery_level <= 100:
        merged["$battery_level"] = battery_level
    merged["$sdk_version"] = SDK_VERSION
    merged["$sdk_platform"] = SDK_PLATFORM

    return {
        "event": event_name,
        "distinct_id": client.identity.distinct_id,
        "timestamp": client.platform.now_ms(),
        "properties": merged,
    }


def encode_event(event):
    return cbor.encode(event)


def decode_event(data):
    value = cbor.decode(data)
    if not isinstance(value, dict):
        raise ValueError("event is not an object")
    return value


def build_batch(api_key, event_bytes):
    events = [decode_event(data) for data in event_bytes]
    return build_batch_from_events(api_key, events)


def build_batch_from_events(api_key, events):
    return cbor.encode({"token": api_key, "batch": events})


def _adapter_properties(client):
    callback = client.config.auto_properties_callback
    if callback is None:
        return {}
    result = callback()
    return require_properties(result)
