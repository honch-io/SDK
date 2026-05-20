#!/usr/bin/env python3
"""Static migration checks for the C/POSIX CBOR wire format."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = ROOT.parent


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


def read_sdk(relative_path: str) -> str:
    return (SDK_ROOT / relative_path).read_text()


class CCoreCborMigrationTest(unittest.TestCase):
    def test_transport_sends_application_cbor_with_optional_gzip(self) -> None:
        transport = read("honch/src/honch_transport_curl.c")
        cmake = read("CMakeLists.txt")

        self.assertIn("Content-Type: application/cbor", transport)
        self.assertNotIn("Content-Type: application/json", transport)
        self.assertIn("Content-Encoding: gzip", transport)
        self.assertIn("honch_gzip_payload", transport)
        self.assertIn("compressed_size < payload_size", transport)
        self.assertIn("find_package(ZLIB", cmake)
        self.assertIn("ZLIB::ZLIB", cmake)

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read("honch/src/honch_queue.c")
        internal = read("honch/src/honch_internal.h")

        self.assertIn('".cbor"', queue)
        self.assertNotIn('".json"', queue)
        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertNotIn("event_json", internal)

    def test_queue_supports_configurable_durability_mode(self) -> None:
        public = read("honch/include/honch/honch.h")
        queue = read("honch/src/honch_queue.c")

        self.assertIn("honch_durability_mode_t", public)
        self.assertIn("durability_mode", public)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS", public)
        self.assertIn("HONCH_DURABILITY_OS_BUFFERED", public)
        self.assertIn("client->durability_mode", queue)

    def test_encoder_builds_cbor_epoch_millis_payloads(self) -> None:
        encoder = read_sdk("core/src/honch_encoder.c")
        honch = read("honch/src/honch.c")
        internal = read("honch/src/honch_internal.h")

        self.assertIn("honch_encoder_build_batch_cbor", internal)
        self.assertIn("honch_cbor_append_text", encoder + honch)
        self.assertIn("honch_now_millis", honch)
        self.assertNotIn("honch_encoder_build_batch_json", internal)
        self.assertNotIn("honch_now_iso8601", honch)

    def test_docs_reference_cbor_contract(self) -> None:
        readme = read("README.md")
        spec = (SDK_ROOT / "spec/wire-format.md").read_text()

        self.assertIn("application/cbor", readme)
        self.assertIn("epoch milliseconds", readme)
        self.assertIn("Content-Type: application/cbor", spec)
        self.assertNotIn("gzip-compressed JSON", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
