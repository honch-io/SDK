#!/usr/bin/env python3
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "spec" / "conformance" / "wire-v2"
COREDUMP_OUT_DIR = ROOT / "spec" / "conformance" / "coredump"
VERSION = 2
SOURCE_EVENTS = 0
SOURCE_COREDUMP = 1
HEADER_CONTINUATION = 0x20
HEADER_MORE = 0x40
KIND_EVENT_BATCH = 1
FLAG_CONTEXT_PROMOTES_PROPERTIES = 1
VALUE_NULL = 0x00
VALUE_FALSE = 0x01
VALUE_TRUE = 0x02
VALUE_UINT = 0x03
VALUE_INT = 0x04
VALUE_FLOAT32 = 0x05
VALUE_FLOAT64 = 0x06
VALUE_STRING_REF = 0x07
VALUE_BYTES = 0x08
VALUE_ARRAY = 0x09
VALUE_MAP = 0x0A
RESERVED_KEYS = {
    "$battery_level": 1,
    "$wifi_rssi": 2,
    "reset_reason": 3,
    "state": 4,
    "previous_version": 5,
    "new_version": 6,
    "session_name": 7,
}
BASE_CONTEXT = {
    "distinct_id": "user-1",
    "$device_id": "device-1",
    "$device_model": "model-x",
    "$firmware_version": "1.0.0",
    "$sdk_platform": "posix",
    "$sdk_version": "0.2.4",
    "$environment": "production",
}
CONTEXT_IDS = {
    "distinct_id": 1,
    "$device_id": 2,
    "$device_model": 3,
    "$firmware_version": 4,
    "$sdk_platform": 5,
    "$sdk_version": 6,
    "$environment": 7,
    "$session_id": 8,
    "device_time_source": 9,
    "capabilities": 10,
    "extension_128": 128,
}
PROMOTED_PROPERTIES = {
    "$device_id",
    "$device_model",
    "$firmware_version",
    "$sdk_platform",
    "$sdk_version",
    "$environment",
    "$session_id",
}


def uvarint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        out.append(byte)
        if not value:
            return bytes(out)


def svarint(value):
    return uvarint((value << 1) ^ (value >> 63))


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def intern(table, value, allow_empty=False):
    if value is None or (not allow_empty and value == ""):
        raise ValueError("invalid string")
    if value not in table:
        table.append(value)
    return table.index(value)


def collect_value_strings(table, value):
    kind = value["type"]
    if kind == "string":
        intern(table, value["value"], allow_empty=True)
    elif kind == "array":
        for item in value["items"]:
            collect_value_strings(table, item)
    elif kind == "map":
        for key, item in value["entries"]:
            collect_key_string(table, key)
            collect_value_strings(table, item)


def collect_key_string(table, key):
    if key in PROMOTED_PROPERTIES:
        raise ValueError(f"promoted context property in event: {key}")
    if key not in RESERVED_KEYS:
        intern(table, key)


def string_table_for(context, events):
    table = []
    for key in (
        "distinct_id",
        "$device_id",
        "$device_model",
        "$firmware_version",
        "$sdk_platform",
        "$sdk_version",
        "$environment",
        "$session_id",
    ):
        if key in context:
            intern(table, context[key])
    for key in ("extension_128",):
        if key in context:
            collect_value_strings(table, context[key])
    for event in events:
        intern(table, event["event"])
        for key, value in event["properties"]:
            collect_key_string(table, key)
            collect_value_strings(table, value)
    return table


def key_ref(table, key):
    if key in RESERVED_KEYS:
        return RESERVED_KEYS[key] << 1
    return (table.index(key) << 1) | 1


def encode_value(table, value):
    kind = value["type"]
    if kind == "null":
        return bytes([VALUE_NULL])
    if kind == "bool":
        return bytes([VALUE_TRUE if value["value"] else VALUE_FALSE])
    if kind == "uint":
        return bytes([VALUE_UINT]) + uvarint(value["value"])
    if kind == "int":
        return bytes([VALUE_INT]) + svarint(value["value"])
    if kind == "float32":
        return bytes([VALUE_FLOAT32]) + struct.pack("<f", value["value"])
    if kind == "float64":
        return bytes([VALUE_FLOAT64]) + struct.pack("<d", value["value"])
    if kind == "string":
        return bytes([VALUE_STRING_REF]) + uvarint(table.index(value["value"]))
    if kind == "bytes":
        raw = bytes.fromhex(value["hex"])
        return bytes([VALUE_BYTES]) + uvarint(len(raw)) + raw
    if kind == "array":
        out = bytearray([VALUE_ARRAY])
        out += uvarint(len(value["items"]))
        for item in value["items"]:
            out += encode_value(table, item)
        return bytes(out)
    if kind == "map":
        out = bytearray([VALUE_MAP])
        out += uvarint(len(value["entries"]))
        for key, item in value["entries"]:
            out += uvarint(key_ref(table, key))
            out += encode_value(table, item)
        return bytes(out)
    raise ValueError(f"unknown value type {kind}")


def encode_message(context, base_time_ms, events, allow_duplicate_properties=False):
    table = string_table_for(context, events)
    out = bytearray()
    out += uvarint(VERSION)
    out += uvarint(KIND_EVENT_BATCH)
    out += uvarint(FLAG_CONTEXT_PROMOTES_PROPERTIES)
    out += uvarint(base_time_ms)
    out += uvarint(len(table))
    for string in table:
        encoded = string.encode("utf-8")
        out += uvarint(len(encoded))
        out += encoded
    context_keys = [key for key in CONTEXT_IDS if key in context]
    out += uvarint(len(context_keys))
    for key in context_keys:
        out += uvarint(CONTEXT_IDS[key])
        if key in {
            "distinct_id",
            "$device_id",
            "$device_model",
            "$firmware_version",
            "$sdk_platform",
            "$sdk_version",
            "$environment",
            "$session_id",
        }:
            out += bytes([VALUE_STRING_REF])
            out += uvarint(table.index(context[key]))
        else:
            out += encode_value(table, context[key])
    out += uvarint(len(events))
    for event in events:
        out += uvarint(table.index(event["event"]))
        out += svarint(event["timestamp_ms"] - base_time_ms)
        seen = set()
        if not allow_duplicate_properties:
            for key, _ in event["properties"]:
                if key in seen:
                    raise ValueError(f"duplicate property key {key}")
                seen.add(key)
        out += uvarint(len(event["properties"]))
        for key, value in event["properties"]:
            out += uvarint(key_ref(table, key))
            out += encode_value(table, value)
    return bytes(out), table


def frame(message, message_id, payload, offset=0, more=False, continuation=False, corrupt_crc=False,
          source_type=SOURCE_EVENTS):
    header = VERSION | (source_type << 2)
    if continuation:
        header |= HEADER_CONTINUATION
    if more:
        header |= HEADER_MORE
    out = bytearray([header])
    out += uvarint(message_id)
    if continuation:
        out += uvarint(offset)
    elif more:
        out += uvarint(len(message))
    out += payload
    if not more:
        crc = crc16_ccitt_false(message)
        if corrupt_crc:
            crc ^= 0x0001
        out += bytes([crc & 0xFF, crc >> 8])
    return bytes(out)


def frames_for(message, message_id, cuts):
    if not cuts:
        return [{"kind": "single", "hex": frame(message, message_id, message).hex()}]
    frames = []
    start = 0
    for index, end in enumerate(cuts + [len(message)]):
        payload = message[start:end]
        more = end < len(message)
        continuation = index != 0
        kind = "init" if index == 0 else "continuation"
        frames.append({
            "kind": kind,
            "hex": frame(message, message_id, payload, offset=start, more=more, continuation=continuation).hex(),
            "offset": start,
        })
        start = end
    return frames


def coredump_frames(blob, message_id, cuts):
    """Frame an opaque coredump blob (source_type=1). The CRC on the final frame
    covers the COMPLETE unchunked blob, identical to the event CRC rule."""
    if not cuts:
        return [{
            "kind": "single",
            "hex": frame(blob, message_id, blob, source_type=SOURCE_COREDUMP).hex(),
            "offset": 0,
        }]
    frames = []
    start = 0
    for index, end in enumerate(cuts + [len(blob)]):
        payload = blob[start:end]
        more = end < len(blob)
        continuation = index != 0
        kind = "init" if index == 0 else ("continuation" if more else "final")
        frames.append({
            "kind": kind,
            "hex": frame(
                blob, message_id, payload, offset=start, more=more,
                continuation=continuation, source_type=SOURCE_COREDUMP).hex(),
            "offset": start,
        })
        start = end
    return frames


def write_coredump_fixture(name, description, blob, message_id, stream_id, cuts=None):
    fixture = {
        "wire_format": "honch-wire-v2",
        "name": name,
        "description": description,
        "generated_by": "tools/generate_wire_v2_fixtures.py",
        "source": "coredump",
        "source_type": SOURCE_COREDUMP,
        "stream_id": stream_id,
        "blob_size": len(blob),
        "blob_hex": blob.hex(),
        "frames": coredump_frames(blob, message_id, cuts or []),
    }
    COREDUMP_OUT_DIR.mkdir(parents=True, exist_ok=True)
    (COREDUMP_OUT_DIR / f"{name}.json").write_text(json.dumps(fixture, indent=2, sort_keys=True) + "\n")


def expand_event(context, event):
    properties = {key: context[key] for key in PROMOTED_PROPERTIES if key in context}
    for key, value in event["properties"]:
        properties[key] = expand_value(value)
    return {
        "event": event["event"],
        "distinct_id": context["distinct_id"],
        "timestamp": event["timestamp_ms"],
        "properties": properties,
    }


def expand_value(value):
    kind = value["type"]
    if kind in {"null", "bool", "uint", "int", "float32", "float64", "string"}:
        return value.get("value")
    if kind == "bytes":
        return {"bytes_hex": value["hex"]}
    if kind == "array":
        return [expand_value(item) for item in value["items"]]
    if kind == "map":
        return {key: expand_value(item) for key, item in value["entries"]}
    raise ValueError(f"unknown value type {kind}")


def decoded(context, base_time_ms, table, events):
    decoded_context = {
        key: expand_value(value) if isinstance(value, dict) and "type" in value else value
        for key, value in context.items()
    }
    return {
        "format_version": 2,
        "message_kind": "event_batch",
        "flags": ["context_promotes_properties"],
        "base_time_ms": base_time_ms,
        "string_table": table,
        "batch_context": decoded_context,
        "events": [
            {
                "event": event["event"],
                "time_delta_ms": event["timestamp_ms"] - base_time_ms,
                "properties": [{key: expand_value(value)} for key, value in event["properties"]],
            }
            for event in events
        ],
    }


def write_fixture(
    name,
    description,
    context,
    base_time_ms,
    events,
    message_id,
    response_code,
    cuts=None,
    extra_frames=None,
    frame_override=None,
    allow_duplicate_properties=False,
):
    message, table = encode_message(context, base_time_ms, events, allow_duplicate_properties=allow_duplicate_properties)
    frames = frame_override(message, message_id) if frame_override else frames_for(message, message_id, cuts or [])
    if extra_frames:
        frames.extend(extra_frames(message, message_id, frames))
    fixture = {
        "wire_format": "honch-wire-v2",
        "name": name,
        "description": description,
        "generated_by": "tools/generate_wire_v2_fixtures.py",
        "expected_capture_response_code": response_code,
        "frames": frames,
        "decoded_compact_message": decoded(context, base_time_ms, table, events),
        "expanded_event_json": [] if response_code >= 400 or response_code == 202 else [expand_event(context, event) for event in events],
    }
    (OUT_DIR / f"{name}.json").write_text(json.dumps(fixture, indent=2, sort_keys=True) + "\n")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_fixture(
        "single-required-context",
        "Single final frame with one event and only required batch context.",
        BASE_CONTEXT,
        1700000000000,
        [{"event": "boot", "timestamp_ms": 1700000000000, "properties": []}],
        1,
        204,
    )
    write_fixture(
        "single-custom-properties",
        "Single final frame with custom properties and repeated string values.",
        BASE_CONTEXT,
        1700000001000,
        [{
            "event": "sample",
            "timestamp_ms": 1700000001025,
            "properties": [
                ("mode", {"type": "string", "value": "auto"}),
                ("previous_mode", {"type": "string", "value": "auto"}),
                ("samples", {"type": "uint", "value": 3}),
                ("tags", {"type": "array", "items": [{"type": "string", "value": "auto"}, {"type": "string", "value": "hdr"}]}),
            ],
        }],
        2,
        204,
    )
    write_fixture(
        "multi-frame-non-value-boundary",
        "Three-frame message split through the compact payload, including a non-value boundary.",
        BASE_CONTEXT,
        1700000002000,
        [{"event": "capture", "timestamp_ms": 1700000002010, "properties": [("mode", {"type": "string", "value": "hdr"})]}],
        3,
        204,
        cuts=[37, 74],
    )
    write_fixture(
        "duplicate-continuation",
        "Duplicate continuation frame whose bytes match an already stored continuation.",
        BASE_CONTEXT,
        1700000003000,
        [{"event": "capture", "timestamp_ms": 1700000003010, "properties": [("mode", {"type": "string", "value": "hdr"})]}],
        4,
        202,
        frame_override=lambda message, message_id: (
            lambda frames: [frames[0], frames[1], {"kind": "duplicate", "hex": frames[1]["hex"], "offset": frames[1]["offset"]}]
        )(frames_for(message, message_id, [37, 74])),
    )
    write_fixture(
        "crc-failure",
        "Final frame with CRC failure; capture must reject before decoding the compact message.",
        BASE_CONTEXT,
        1700000004000,
        [{"event": "boot", "timestamp_ms": 1700000004000, "properties": []}],
        5,
        400,
        frame_override=lambda message, message_id: [{"kind": "corrupt-final", "hex": frame(message, message_id, message, corrupt_crc=True).hex()}],
    )
    write_fixture(
        "duplicate-reserved-property",
        "Semantic failure: duplicate reserved property keys in one event.",
        BASE_CONTEXT,
        1700000005000,
        [{
            "event": "battery",
            "timestamp_ms": 1700000005000,
            "properties": [
                ("$battery_level", {"type": "int", "value": 44}),
                ("$battery_level", {"type": "int", "value": 45}),
            ],
        }],
        6,
        422,
        allow_duplicate_properties=True,
    )
    max_events = [
        {"event": "sample", "timestamp_ms": 1700000006000 + i, "properties": [("count", {"type": "uint", "value": i})]}
        for i in range(50)
    ]
    write_fixture(
        "maximum-sdk-limit",
        "Maximum official SDK event-count fixture: 50 events in one compact message.",
        BASE_CONTEXT,
        1700000006000,
        max_events,
        7,
        204,
    )
    write_fixture(
        "value-types",
        "Single final frame covering bytes, float32, and float64 scalar values. Capture rejects bytes unless binary properties are enabled.",
        BASE_CONTEXT,
        1700000007000,
        [{
            "event": "reading",
            "timestamp_ms": 1700000007000,
            "properties": [
                ("payload", {"type": "bytes", "hex": "00ff10"}),
                ("temperature_f32", {"type": "float32", "value": 36.5}),
                ("voltage_f64", {"type": "float64", "value": 3.141592653589793}),
            ],
        }],
        8,
        422,
    )
    write_fixture(
        "metadata-context",
        "Single final frame with optional ingest metadata context keys that are accepted but not promoted to event properties.",
        {
            **BASE_CONTEXT,
            "device_time_source": {"type": "uint", "value": 1},
            "capabilities": {"type": "uint", "value": 3},
            "extension_128": {"type": "map", "entries": [("gateway_batch", {"type": "string", "value": "alpha"})]},
        },
        1700000008000,
        [{"event": "boot", "timestamp_ms": 1700000008000, "properties": []}],
        9,
        204,
    )

    # Coredump (source_type=1) opaque-blob fixtures. The body is raw bytes, not a
    # compact event message; the CRC on the final frame still covers the complete
    # unchunked blob. These pin the cross-SDK framing of a raw coredump upload.
    small_blob = bytes((i * 31 + 7) & 0xFF for i in range(200))
    write_coredump_fixture(
        "coredump-single-frame",
        "A small coredump blob that fits in one frame: continuation=0, more=0, CRC over the whole blob.",
        small_blob,
        0x1000,
        "crash-7f3a9c2e",
    )
    # 1300-byte blob chunked at the SDK's 512-byte boundary: offsets 0, 512, 1024.
    big_blob = bytes((i * 7 + 3) & 0xFF for i in range(1300))
    write_coredump_fixture(
        "coredump-multi-frame",
        "A 1300-byte coredump streamed as init(512)+continuation(512)+final(276); the final frame carries the CRC of the complete 1300-byte blob.",
        big_blob,
        0x1001,
        "crash-91d4b60a",
        cuts=[512, 1024],
    )


if __name__ == "__main__":
    main()
