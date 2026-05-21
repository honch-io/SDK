#!/usr/bin/env python3
"""Static migration checks for the C/POSIX CBOR wire format."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = ROOT.parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


def read_sdk(relative_path: str) -> str:
    return (SDK_ROOT / relative_path).read_text()


class PosixCborMigrationTest(unittest.TestCase):
    def test_core_status_header_exposes_guarded_short_aliases(self) -> None:
        status = read_sdk("core/include/honch/core/status.h")

        self.assertIn("HONCH_STATUS_OK = 0", status)
        self.assertIn("HONCH_STATUS_ERROR_INVALID_ARGUMENT = 1", status)
        self.assertIn("#ifndef HONCH_CORE_NO_SHORT_STATUS_NAMES", status)
        self.assertIn("#define HONCH_OK HONCH_STATUS_OK", status)
        self.assertIn("#define HONCH_ERROR_INVALID_ARGUMENT HONCH_STATUS_ERROR_INVALID_ARGUMENT", status)

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

    def test_transport_guards_curl_post_field_size_cast(self) -> None:
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")

        self.assertIn("body_size > (size_t)LONG_MAX", transport)
        self.assertIn("CURLOPT_POSTFIELDSIZE, (long)body_size", transport)
        self.assertLess(
            transport.index("body_size > (size_t)LONG_MAX"),
            transport.index("CURLOPT_POSTFIELDSIZE, (long)body_size"),
        )

    def test_transport_builds_batch_url_without_int_length_cast(self) -> None:
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")
        batch_url = transport[
            transport.index("static honch_status_t honch_batch_url"):
            transport.index("static honch_status_t honch_map_response")
        ]

        self.assertNotIn("(int)endpoint_length", batch_url)
        self.assertNotIn("%.*s", batch_url)
        self.assertIn("memcpy(url, endpoint_url, endpoint_length)", batch_url)
        self.assertIn("memcpy(url + endpoint_length, suffix, suffix_length)", batch_url)

    def test_queue_persists_cbor_blobs_not_json_strings(self) -> None:
        queue = read_sdk("ports/posix/src/posix_storage.c")
        internal = read_sdk("core/src/honch_internal.h")

        self.assertIn('".cbor"', queue)
        self.assertNotIn('".json"', queue)
        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertNotIn("event_json", internal)

    def test_queue_supports_configurable_durability_mode(self) -> None:
        public = read("include/honch/honch.h") + read_sdk("core/include/honch/core/config.h")
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
        posix_cmake = read("CMakeLists.txt")
        core_cmake = read_sdk("core/CMakeLists.txt")

        self.assertIn("src/honch_core.c", core_cmake)
        self.assertNotIn("honch/src/honch.c", posix_cmake)
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

    def test_core_routes_public_locking_through_platform_ops_when_available(self) -> None:
        core = read_sdk("core/src/honch_core.c")
        lifecycle = read_sdk("core/src/honch_lifecycle.c")
        platform = read_sdk("ports/posix/src/posix_platform.c")

        self.assertIn("next->platform_ops.ctx = next", core)
        self.assertIn("honch_client_lock", core)
        self.assertIn("honch_client_state_lock(client)", core)
        self.assertIn("client->platform->lock", lifecycle)
        self.assertIn("honch_client_unlock", core)
        self.assertIn("honch_client_state_unlock(client)", core)
        self.assertIn("client->platform->unlock", lifecycle)
        self.assertIn("honch_client_lock(client)", core)
        self.assertIn("honch_client_unlock(client)", core)

        self.assertIn("static honch_status_t honch_posix_lock", platform)
        self.assertIn("static honch_status_t honch_posix_unlock", platform)
        self.assertIn("honch_client_t *client = (honch_client_t *)ctx", platform)
        self.assertIn(".ctx = NULL", platform)

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

    def test_flush_routes_batch_posts_through_transport_ops_when_available(self) -> None:
        core = read_sdk("core/src/honch_core.c")
        queue_policy = read_sdk("core/src/honch_queue_policy.c")
        storage = read_sdk("ports/posix/src/posix_storage.c")
        transport = read_sdk("ports/posix/src/posix_transport_curl.c")

        self.assertIn("next->transport_ops.ctx = next", core)
        self.assertIn("honch_core_post_batch", queue_policy)
        self.assertIn("client->transport->post_batch", queue_policy)
        self.assertIn("honch_core_queue_peek", queue_policy)
        self.assertIn("honch_core_queue_consume", queue_policy)
        self.assertIn("honch_core_queue_dead_letter", queue_policy)
        self.assertIn(".queue_peek = honch_posix_queue_peek", storage)
        self.assertIn(".queue_consume = honch_posix_queue_consume", storage)
        self.assertIn(".queue_dead_letter = honch_posix_queue_dead_letter", storage)
        self.assertIn("honch_client_t *client = (honch_client_t *)ctx", transport)

    def test_posix_state_helpers_route_through_storage_ops_when_available(self) -> None:
        state = read_sdk("ports/posix/src/posix_state.c")
        storage = read_sdk("ports/posix/src/posix_storage.c")

        self.assertIn("client->storage->state_get", state)
        self.assertIn("client->storage->state_set", state)

        self.assertIn(".state_get = honch_posix_state_get", storage)
        self.assertIn(".state_set = honch_posix_state_set", storage)
        self.assertIn(".state_delete = honch_posix_state_delete", storage)
        self.assertIn("honch_client_t *client = (honch_client_t *)ctx", storage)

    def test_docs_reference_cbor_contract(self) -> None:
        readme = read("README.md")
        spec = (SDK_ROOT / "spec/wire-format.md").read_text()

        self.assertIn("application/cbor", readme)
        self.assertIn("epoch milliseconds", readme)
        self.assertIn("Content-Type: application/cbor", spec)
        self.assertNotIn("gzip-compressed JSON", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
