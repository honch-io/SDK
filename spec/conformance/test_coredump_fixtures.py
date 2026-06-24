"""Cross-SDK conformance for the coredump (source_type=1) wire framing.

A coredump upload reuses the wire-v2 multi-frame chunk machinery but the body is
an OPAQUE binary blob, not a compact event message. These fixtures pin the
framing every SDK must produce and every receiver must accept:

  * header version=2, source_type=1, reserved bit clear
  * init frame carries total_message_length, continuations carry offset
  * the final frame clears `more` and carries a CRC-16/CCITT-FALSE over the
    COMPLETE unchunked blob (identical CRC rule to events)
  * reassembling the frame payloads in offset order reproduces the blob exactly

Run from the repo root:
    python3 -m unittest spec.conformance.test_coredump_fixtures
"""

import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
COREDUMP_DIR = ROOT / "coredump"
REQUIRED_FIXTURES = {
    "coredump-single-frame": ["single"],
    "coredump-multi-frame": ["init", "continuation", "final"],
}
HEX_RE = re.compile(r"^[0-9a-f]+$")
HEADER_CONTINUATION = 0x20
HEADER_MORE = 0x40
SOURCE_COREDUMP = 1


class CoredumpFixtureTest(unittest.TestCase):
    def test_required_fixtures_exist(self):
        self.assertTrue(COREDUMP_DIR.is_dir(), "spec/conformance/coredump is missing")
        for name in REQUIRED_FIXTURES:
            with self.subTest(fixture=name):
                self.assertTrue((COREDUMP_DIR / f"{name}.json").is_file())

    def test_fixture_schema(self):
        for name, frame_kinds in REQUIRED_FIXTURES.items():
            with self.subTest(fixture=name):
                fixture = json.loads((COREDUMP_DIR / f"{name}.json").read_text())
                self.assertEqual(fixture["wire_format"], "honch-wire-v2")
                self.assertEqual(fixture["name"], name)
                self.assertEqual(fixture["source"], "coredump")
                self.assertEqual(fixture["source_type"], SOURCE_COREDUMP)
                self.assertIsInstance(fixture["stream_id"], str)
                self.assertGreater(len(fixture["stream_id"]), 0)
                self.assertRegex(fixture["blob_hex"], HEX_RE)
                self.assertEqual(len(fixture["blob_hex"]), fixture["blob_size"] * 2)
                self.assertEqual([f["kind"] for f in fixture["frames"]], frame_kinds)
                for f in fixture["frames"]:
                    self.assertIsInstance(f["hex"], str)
                    self.assertEqual(len(f["hex"]) % 2, 0)
                    self.assertRegex(f["hex"], HEX_RE)

    def test_frame_integrity_and_reassembly(self):
        for name in REQUIRED_FIXTURES:
            with self.subTest(fixture=name):
                fixture = json.loads((COREDUMP_DIR / f"{name}.json").read_text())
                blob = bytes.fromhex(fixture["blob_hex"])
                parsed = [parse_frame(bytes.fromhex(f["hex"])) for f in fixture["frames"]]

                for p in parsed:
                    self.assertEqual(p["version"], 2)
                    self.assertEqual(p["source_type"], SOURCE_COREDUMP)
                    self.assertFalse(p["reserved"])

                init = parsed[0]
                self.assertFalse(init["continuation"])
                if len(parsed) > 1:
                    # Multi-frame: the init declares the full blob length.
                    self.assertEqual(init["total_message_length"], len(blob))
                    for p in parsed[1:]:
                        self.assertTrue(p["continuation"])

                # Offsets are contiguous and reassembly reproduces the blob.
                reassembled = reassemble(parsed)
                self.assertEqual(reassembled, blob)

                # Exactly one final frame (more=0) carrying the whole-blob CRC.
                final = parsed[-1]
                self.assertFalse(final["more"])
                self.assertEqual(sum(1 for p in parsed if not p["more"]), 1)
                self.assertEqual(final["crc"], crc16_ccitt_false(blob))


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


if __name__ == "__main__":
    unittest.main()
