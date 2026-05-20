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
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")
        cmake = read("CMakeLists.txt") + read_sdk("ports/posix/CMakeLists.txt")

        self.assertIn("Content-Type: application/cbor", transport)
        self.assertNotIn("Content-Type: application/json", transport)
        self.assertIn("Content-Encoding: gzip", transport)
        self.assertIn("honch_gzip_payload", transport)
        self.assertIn("compressed_size < payload_size", transport)
        self.assertIn("find_package(ZLIB", cmake)
        self.assertIn("ZLIB::ZLIB", cmake)

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read_sdk("ports/posix/src/posix_storage.c")
        internal = read_sdk("core/src/honch_internal.h")

        self.assertIn('".cbor"', queue)
        self.assertNotIn('".json"', queue)
        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertNotIn("event_json", internal)

    def test_queue_supports_configurable_durability_mode(self) -> None:
        public = read("honch/include/honch/honch.h") + read_sdk("core/include/honch/core/config.h")
        queue = read_sdk("ports/posix/src/posix_storage.c")

        self.assertIn("honch_durability_mode_t", public)
        self.assertIn("durability_mode", public)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS", public)
        self.assertIn("HONCH_DURABILITY_OS_BUFFERED", public)
        self.assertIn("client->durability_mode", queue)

    def test_encoder_builds_cbor_epoch_millis_payloads(self) -> None:
        encoder = read_sdk("core/src/honch_encoder.c")
        honch = read_sdk("core/src/honch_core.c")
        internal = read_sdk("core/src/honch_internal.h")

        self.assertIn("honch_encoder_build_batch_cbor", internal)
        self.assertIn("honch_cbor_append_text", encoder + honch)
        self.assertIn("honch_now_millis", honch)
        self.assertNotIn("honch_encoder_build_batch_json", internal)
        self.assertNotIn("honch_now_iso8601", honch)

    def test_client_state_machine_lives_in_core_with_posix_wrappers(self) -> None:
        self.assertTrue((SDK_ROOT / "core/src/honch_core.c").exists())
        self.assertTrue((SDK_ROOT / "ports/posix/src/posix_compat.c").exists())

        core = read_sdk("core/src/honch_core.c")
        compat = read_sdk("ports/posix/src/posix_compat.c")
        c_core_cmake = read("CMakeLists.txt")
        core_cmake = read_sdk("core/CMakeLists.txt")
        posix_cmake = read_sdk("ports/posix/CMakeLists.txt")

        self.assertIn("src/honch_core.c", core_cmake)
        self.assertNotIn("honch/src/honch.c", c_core_cmake)
        self.assertIn("src/posix_compat.c", posix_cmake)

        self.assertIn("honch_core_init", core)
        self.assertIn("honch_core_track", core)
        self.assertNotIn("honch_status_t honch_init", core)
        self.assertIn("honch_status_t honch_init", compat)
        self.assertIn("honch_core_init", compat)
        self.assertNotIn("(const honch_core_config_t *)config", compat)

    def test_core_client_owns_ops_and_posix_compat_initializes_them(self) -> None:
        internal = read_sdk("core/src/honch_internal.h")
        core = read_sdk("core/src/honch_core.c")
        compat = read_sdk("ports/posix/src/posix_compat.c")
        posix_header = read_sdk("ports/posix/include/honch/posix/honch.h")
        platform = read_sdk("ports/posix/src/posix_platform.c")
        storage = read_sdk("ports/posix/src/posix_storage.c")
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")

        self.assertIn("honch_platform_ops_t platform_ops", internal)
        self.assertIn("const honch_platform_ops_t *platform", internal)
        self.assertIn("honch_storage_ops_t storage_ops", internal)
        self.assertIn("const honch_storage_ops_t *storage", internal)
        self.assertIn("honch_transport_ops_t transport_ops", internal)
        self.assertIn("const honch_transport_ops_t *transport", internal)

        self.assertIn("next->platform_ops = *config->platform", core)
        self.assertIn("next->storage_ops = *config->storage", core)
        self.assertIn("next->transport_ops = *config->transport", core)

        self.assertIn("honch_posix_platform_t", posix_header)
        self.assertIn("honch_posix_storage_t", posix_header)
        self.assertIn("honch_posix_transport_t", posix_header)
        self.assertIn("honch_posix_platform_ops_init", platform)
        self.assertIn("honch_posix_storage_ops_init", storage)
        self.assertIn("honch_posix_transport_ops_init", transport)
        self.assertIn("honch_posix_platform_ops_init(&platform_ops", compat)
        self.assertIn("honch_posix_storage_ops_init(&storage_ops", compat)
        self.assertIn("honch_posix_transport_ops_init(&transport_ops", compat)

    def test_core_uses_platform_ops_for_time_and_random_when_available(self) -> None:
        core = read_sdk("core/src/honch_core.c")

        self.assertIn("honch_client_now_millis", core)
        self.assertIn("honch_client_random_hex", core)
        self.assertIn("client->platform->now_ms", core)
        self.assertIn("client->platform->random_bytes", core)
        self.assertIn("honch_client_now_millis(client)", core)
        self.assertIn("honch_client_random_hex(client, random)", core)

    def test_core_routes_queue_mutations_through_storage_ops_when_available(self) -> None:
        core = read_sdk("core/src/honch_core.c")
        storage = read_sdk("ports/posix/src/posix_storage.c")

        self.assertIn("honch_client_queue_push", core)
        self.assertIn("client->storage->queue_push", core)
        self.assertIn("honch_client_queue_depth", core)
        self.assertIn("client->storage->queue_depth", core)
        self.assertIn("honch_client_queue_clear", core)
        self.assertIn("client->storage->queue_clear", core)

        self.assertIn("honch_client_queue_push(client, event.data, event.length)", core)
        self.assertIn("honch_client_queue_depth(next, &next->queued_event_count)", core)
        self.assertIn("honch_client_queue_clear(client)", core)
        self.assertIn("client->sequence++", core)
        self.assertIn("honch_client_t *client = (honch_client_t *)ctx", storage)
        self.assertIn("honch_queue_enqueue_with_sequence(client, event, event_size, sequence)", storage)

    def test_docs_reference_cbor_contract(self) -> None:
        readme = read("README.md")
        spec = (SDK_ROOT / "spec/wire-format.md").read_text()

        self.assertIn("application/cbor", readme)
        self.assertIn("epoch milliseconds", readme)
        self.assertIn("Content-Type: application/cbor", spec)
        self.assertNotIn("gzip-compressed JSON", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
