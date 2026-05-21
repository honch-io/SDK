import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
WIRE_V2_DIR = ROOT / "wire-v2"
REQUIRED_FIXTURES = {
    "single-required-context": 204,
    "single-custom-properties": 204,
    "multi-frame-non-value-boundary": 204,
    "duplicate-continuation": 202,
    "crc-failure": 400,
    "duplicate-reserved-property": 422,
    "maximum-sdk-limit": 204,
    "metadata-context": 204,
    "value-types": 422,
}
EXPECTED_FRAME_KINDS = {
    "single-required-context": ["single"],
    "single-custom-properties": ["single"],
    "multi-frame-non-value-boundary": ["init", "continuation", "continuation"],
    "duplicate-continuation": ["init", "continuation", "duplicate"],
    "crc-failure": ["corrupt-final"],
    "duplicate-reserved-property": ["single"],
    "maximum-sdk-limit": ["single"],
    "metadata-context": ["single"],
    "value-types": ["single"],
}
HEX_RE = re.compile(r"^[0-9a-f]+$")
HEADER_CONTINUATION = 0x20
HEADER_MORE = 0x40
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
CONTEXT_KEYS = {
    1: "distinct_id",
    2: "$device_id",
    3: "$device_model",
    4: "$firmware_version",
    5: "$sdk_platform",
    6: "$sdk_version",
    7: "$environment",
    8: "$session_id",
    9: "device_time_source",
    10: "capabilities",
    128: "extension_128",
}
RESERVED_KEYS = {
    1: "$battery_level",
    2: "$wifi_rssi",
    3: "reset_reason",
    4: "state",
    5: "previous_version",
    6: "new_version",
    7: "session_name",
}


class WireV2FixtureTest(unittest.TestCase):
    def test_required_wire_v2_fixtures_exist(self):
        self.assertTrue(WIRE_V2_DIR.is_dir(), "spec/conformance/wire-v2 is missing")
        for name in REQUIRED_FIXTURES:
            with self.subTest(fixture=name):
                self.assertTrue((WIRE_V2_DIR / f"{name}.json").is_file())

    def test_wire_v2_fixture_schema(self):
        for name, response_code in REQUIRED_FIXTURES.items():
            with self.subTest(fixture=name):
                fixture = json.loads((WIRE_V2_DIR / f"{name}.json").read_text())
                self.assertEqual(fixture["wire_format"], "honch-wire-v2")
                self.assertEqual(fixture["name"], name)
                self.assertEqual(fixture["expected_capture_response_code"], response_code)
                self.assertIsInstance(fixture["frames"], list)
                self.assertGreaterEqual(len(fixture["frames"]), 1)
                self.assertEqual([frame["kind"] for frame in fixture["frames"]], EXPECTED_FRAME_KINDS[name])
                decoded = fixture["decoded_compact_message"]
                if name == "value-types":
                    properties = decoded["events"][0]["properties"]
                    self.assertIn({"payload": {"bytes_hex": "00ff10"}}, properties)
                    self.assertIn({"temperature_f32": 36.5}, properties)
                    self.assertIn({"voltage_f64": 3.141592653589793}, properties)
                for frame in fixture["frames"]:
                    self.assertIn(frame["kind"], {"single", "init", "continuation", "duplicate", "corrupt-final"})
                    self.assertIsInstance(frame["hex"], str)
                    self.assertEqual(len(frame["hex"]) % 2, 0)
                    self.assertRegex(frame["hex"], HEX_RE)
                self.assertEqual(decoded["format_version"], 2)
                self.assertEqual(decoded["message_kind"], "event_batch")
                self.assertIn("string_table", decoded)
                self.assertIn("batch_context", decoded)
                self.assertIn("events", decoded)
                self.assertIsInstance(fixture["expanded_event_json"], list)

    def test_wire_v2_frame_integrity(self):
        for name in REQUIRED_FIXTURES:
            with self.subTest(fixture=name):
                fixture = json.loads((WIRE_V2_DIR / f"{name}.json").read_text())
                parsed_frames = [parse_frame(bytes.fromhex(frame["hex"])) for frame in fixture["frames"]]
                for parsed in parsed_frames:
                    self.assertEqual(parsed["version"], 2)
                    self.assertEqual(parsed["source_type"], 0)
                    self.assertFalse(parsed["reserved"])

                if name == "duplicate-continuation":
                    self.assertEqual(parsed_frames[1]["raw"], parsed_frames[2]["raw"])
                    continue

                if name == "crc-failure":
                    self.assertFalse(frame_crc_valid(parsed_frames[0]["payload"], parsed_frames[0]))
                    continue

                message = reassemble(parsed_frames)
                final = parsed_frames[-1]
                self.assertFalse(final["more"])
                self.assertTrue(frame_crc_valid(message, final))

    def test_wire_v2_decoded_compact_message_matches_frame_payload(self):
        for name in REQUIRED_FIXTURES:
            if name == "duplicate-continuation":
                continue
            with self.subTest(fixture=name):
                fixture = json.loads((WIRE_V2_DIR / f"{name}.json").read_text())
                parsed_frames = [parse_frame(bytes.fromhex(frame["hex"])) for frame in fixture["frames"]]
                message = reassemble(parsed_frames)
                self.assertEqual(decode_compact_message(message), fixture["decoded_compact_message"])

def read_uvarint(data, offset):
    value = 0
    shift = 0
    for _ in range(10):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, offset
        shift += 7
    raise ValueError("malformed varint")


def read_svarint(data, offset):
    encoded, offset = read_uvarint(data, offset)
    return (encoded >> 1) ^ -(encoded & 1), offset


def parse_frame(data):
    header = data[0]
    offset = 1
    message_id, offset = read_uvarint(data, offset)
    continuation = (header & HEADER_CONTINUATION) != 0
    more = (header & HEADER_MORE) != 0
    declared_offset = 0
    total_message_length = None
    if continuation:
        declared_offset, offset = read_uvarint(data, offset)
    elif more:
        total_message_length, offset = read_uvarint(data, offset)
    crc = None
    payload_end = len(data)
    if not more:
        crc = data[-2] | (data[-1] << 8)
        payload_end -= 2
    return {
        "raw": data,
        "version": header & 0x03,
        "source_type": (header >> 2) & 0x07,
        "reserved": (header & 0x80) != 0,
        "message_id": message_id,
        "continuation": continuation,
        "more": more,
        "offset": declared_offset,
        "total_message_length": total_message_length,
        "payload": data[offset:payload_end],
        "crc": crc,
    }


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


def frame_crc_valid(message, frame):
    return frame["crc"] == crc16_ccitt_false(message)


def reassemble(frames):
    message = bytearray()
    for frame in frames:
        if frame["continuation"]:
            if frame["offset"] != len(message):
                raise AssertionError(f"offset mismatch: expected {len(message)}, got {frame['offset']}")
        elif frame["offset"] != 0:
            raise AssertionError("init frame offset must be zero")
        message.extend(frame["payload"])
    return bytes(message)


def decode_key_ref(table, encoded):
    if encoded == 0:
        raise ValueError("invalid key ref")
    if encoded & 1:
        index = encoded >> 1
        return table[index]
    return RESERVED_KEYS[encoded >> 1]


def decode_value(data, offset, table):
    tag = data[offset]
    offset += 1
    if tag == VALUE_NULL:
        return None, offset
    if tag == VALUE_FALSE:
        return False, offset
    if tag == VALUE_TRUE:
        return True, offset
    if tag == VALUE_UINT:
        return read_uvarint(data, offset)
    if tag == VALUE_INT:
        return read_svarint(data, offset)
    if tag == VALUE_FLOAT32:
        import struct
        return struct.unpack("<f", data[offset:offset + 4])[0], offset + 4
    if tag == VALUE_FLOAT64:
        import struct
        return struct.unpack("<d", data[offset:offset + 8])[0], offset + 8
    if tag == VALUE_STRING_REF:
        index, offset = read_uvarint(data, offset)
        return table[index], offset
    if tag == VALUE_BYTES:
        length, offset = read_uvarint(data, offset)
        return {"bytes_hex": data[offset:offset + length].hex()}, offset + length
    if tag == VALUE_ARRAY:
        count, offset = read_uvarint(data, offset)
        items = []
        for _ in range(count):
            item, offset = decode_value(data, offset, table)
            items.append(item)
        return items, offset
    if tag == VALUE_MAP:
        count, offset = read_uvarint(data, offset)
        entries = {}
        for _ in range(count):
            key_encoded, offset = read_uvarint(data, offset)
            key = decode_key_ref(table, key_encoded)
            value, offset = decode_value(data, offset, table)
            entries[key] = value
        return entries, offset
    raise ValueError(f"reserved value tag {tag}")


def decode_compact_message(data):
    offset = 0
    format_version, offset = read_uvarint(data, offset)
    kind, offset = read_uvarint(data, offset)
    flags, offset = read_uvarint(data, offset)
    base_time_ms, offset = read_uvarint(data, offset)
    string_count, offset = read_uvarint(data, offset)
    table = []
    for _ in range(string_count):
        length, offset = read_uvarint(data, offset)
        table.append(data[offset:offset + length].decode("utf-8"))
        offset += length

    context_count, offset = read_uvarint(data, offset)
    context = {}
    for _ in range(context_count):
        key_id, offset = read_uvarint(data, offset)
        value, offset = decode_value(data, offset, table)
        context[CONTEXT_KEYS.get(key_id, str(key_id))] = value

    event_count, offset = read_uvarint(data, offset)
    events = []
    for _ in range(event_count):
        event_name_ref, offset = read_uvarint(data, offset)
        delta, offset = read_svarint(data, offset)
        property_count, offset = read_uvarint(data, offset)
        properties = []
        for _ in range(property_count):
            key_encoded, offset = read_uvarint(data, offset)
            key = decode_key_ref(table, key_encoded)
            value, offset = decode_value(data, offset, table)
            properties.append({key: value})
        events.append({
            "event": table[event_name_ref],
            "time_delta_ms": delta,
            "properties": properties,
        })

    if offset != len(data):
        raise ValueError("trailing bytes")
    return {
        "format_version": format_version,
        "message_kind": "event_batch" if kind == 1 else kind,
        "flags": ["context_promotes_properties"] if flags == 1 else flags,
        "base_time_ms": base_time_ms,
        "string_table": table,
        "batch_context": context,
        "events": events,
    }


if __name__ == "__main__":
    unittest.main()
