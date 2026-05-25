#!/usr/bin/env python3
"""Static checks for the C/POSIX default chunk wire transport."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = ROOT.parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


def read_sdk(relative_path: str) -> str:
    return (SDK_ROOT / relative_path).read_text()


class PosixChunkWireTest(unittest.TestCase):
    def test_core_status_header_exposes_guarded_short_aliases(self) -> None:
        status = read_sdk("core/include/honch/core/status.h")

        self.assertIn("HONCH_STATUS_OK = 0", status)
        self.assertIn("HONCH_STATUS_ERROR_INVALID_ARGUMENT = 1", status)
        self.assertIn("#ifndef HONCH_CORE_NO_SHORT_STATUS_NAMES", status)
        self.assertIn("#define HONCH_OK HONCH_STATUS_OK", status)

    def test_transport_posts_chunk_wire_to_capture_endpoint(self) -> None:
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")
        cmake = read("CMakeLists.txt") + read_sdk("ports/posix/CMakeLists.txt")

        self.assertIn('"/capture"', transport)
        self.assertIn("Content-Type: application/vnd.honch.chunk", transport)
        self.assertIn("X-Honch-Project-Key", transport)
        self.assertIn("X-Honch-Stream-Id", transport)
        self.assertIn("HONCH_TRANSPORT_CHUNK_STORED", transport)
        self.assertIn(".post_chunk = honch_posix_transport_post_chunk_ops", transport)
        self.assertNotIn("post_batch", transport)
        self.assertNotIn("Content-Type: application/cbor", transport)
        self.assertNotIn("Content-Encoding: gzip", transport)
        self.assertNotIn("find_package(ZLIB", cmake)

    def test_transport_guards_curl_post_field_size_cast(self) -> None:
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")

        self.assertIn("payload_size > (size_t)LONG_MAX", transport)
        self.assertIn("CURLOPT_POSTFIELDSIZE, (long)payload_size", transport)
        self.assertLess(
            transport.index("payload_size > (size_t)LONG_MAX"),
            transport.index("CURLOPT_POSTFIELDSIZE, (long)payload_size"),
        )

    def test_queue_persists_events_for_compact_encoding(self) -> None:
        queue = read_sdk("ports/posix/src/posix_storage.c")
        internal = read_sdk("core/src/honch_internal.h")
        queue_policy = read_sdk("core/src/honch_queue_policy.c")

        self.assertIn('".cbor"', queue)
        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertIn("honch_core_build_wire_v2_message", queue_policy)
        self.assertIn("honch_core_post_wire_v2_message", queue_policy)
        self.assertIn("client->transport->post_chunk", queue_policy)
        self.assertNotIn("honch_core_post_batch", queue_policy)

    def test_queue_supports_configurable_durability_mode(self) -> None:
        public = read("include/honch/honch.h") + read_sdk("core/include/honch/core/config.h")
        queue = read_sdk("ports/posix/src/posix_storage.c")

        self.assertIn("honch_durability_mode_t", public)
        self.assertIn("durability_mode", public)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS", public)
        self.assertIn("HONCH_DURABILITY_OS_BUFFERED", public)
        self.assertIn("client->durability_mode", queue)

    def test_client_state_machine_lives_in_core_with_posix_wrappers(self) -> None:
        core = read_sdk("core/src/honch_core.c")
        compat = read_sdk("ports/posix/src/posix_compat.c")
        posix_cmake = read("CMakeLists.txt")
        core_cmake = read_sdk("core/CMakeLists.txt")

        self.assertIn("src/honch_core.c", core_cmake)
        self.assertNotIn("honch/src/honch.c", posix_cmake)
        self.assertIn("src/posix_compat.c", posix_cmake)
        self.assertIn("honch_core_init", core)
        self.assertIn("honch_core_track", core)
        self.assertIn("honch_status_t honch_init", compat)
        self.assertIn("honch_core_init", compat)

    def test_public_config_has_no_legacy_wire_toggles(self) -> None:
        public = read("include/honch/honch.h") + read_sdk("core/include/honch/core/config.h")
        compat = read_sdk("ports/posix/src/posix_compat.c")

        self.assertNotIn("enable_wire_v2", public + compat)
        self.assertNotIn("disable_gzip", public + compat)
        self.assertNotIn("gzip_min_bytes", public + compat)

    def test_docs_reference_chunk_wire_contract(self) -> None:
        readme = read("README.md")
        spec = (SDK_ROOT / "spec/wire-format-v2.md").read_text()

        self.assertIn("/capture", readme)
        self.assertIn("application/vnd.honch.chunk", readme)
        self.assertIn("POST /capture", spec)
        self.assertNotIn("application/cbor", readme)
        self.assertNotIn("POST /batch", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
