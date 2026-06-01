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


def c_function_body(source: str, name: str) -> str:
    marker = f"{name}("
    signature_start = source.find(marker)
    if signature_start < 0:
        raise AssertionError(f"function {name} not found")

    open_brace = source.find("{", signature_start)
    if open_brace < 0:
        raise AssertionError(f"function {name} body not found")

    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace + 1:index]

    raise AssertionError(f"function {name} body is not closed")


class PosixChunkWireTest(unittest.TestCase):
    def test_core_status_header_exposes_guarded_short_aliases(self) -> None:
        status = read_sdk("core/include/honch/core/status.h")

        self.assertIn("HONCH_STATUS_OK = 0", status)
        self.assertIn("HONCH_STATUS_ERROR_INVALID_ARGUMENT = 1", status)
        self.assertIn("HONCH_STATUS_ERROR_BUSY", status)
        self.assertIn("#ifndef HONCH_CORE_NO_SHORT_STATUS_NAMES", status)
        self.assertIn("#define HONCH_OK HONCH_STATUS_OK", status)

    def test_core_is_pthread_free_and_exposes_cooperative_tick(self) -> None:
        core_header = read_sdk("core/include/honch/core/honch.h")
        platform_header = read_sdk("core/include/honch/core/platform.h")
        config_header = read_sdk("core/include/honch/core/config.h")
        internal = read_sdk("core/src/honch_internal.h")
        core = read_sdk("core/src/honch_core.c")
        lifecycle = read_sdk("core/src/honch_lifecycle.c")

        self.assertIn("honch_core_tick", core_header)
        self.assertIn("mutex_create", platform_header)
        self.assertIn("mutex_lock", platform_header)
        self.assertNotIn("disable_background_flush", config_header)

        portable_core = "\n".join([internal, core, lifecycle])
        self.assertNotIn("#include <pthread.h>", portable_core)
        self.assertNotIn("pthread_", portable_core)
        self.assertNotIn("pthread_t", portable_core)
        self.assertNotIn("CLOCK_REALTIME", portable_core)
        self.assertNotIn("pthread_create", portable_core)

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

        self.assertIn('".hqe"', queue)
        self.assertIn("const unsigned char *event", internal)
        self.assertIn("size_t event_size", internal)
        self.assertIn("honch_core_build_wire_v2_message", queue_policy)
        self.assertIn("honch_core_post_wire_v2_message", queue_policy)
        self.assertIn("client->transport->post_chunk", queue_policy)
        self.assertNotIn("honch_core_post_batch", queue_policy)

    def test_flush_and_packetizer_share_single_event_wire_encoder(self) -> None:
        internal = read_sdk("core/src/honch_internal.h")
        queue_policy = read_sdk("core/src/honch_queue_policy.c")
        packetizer = read_sdk("core/src/honch_packetizer.c")

        self.assertIn("honch_core_encode_single_wire_v2_event", internal)
        self.assertIn("honch_core_encode_single_wire_v2_event", queue_policy)
        self.assertIn("honch_core_encode_single_wire_v2_event", packetizer)
        self.assertNotIn("honch_packetizer_normalize_timestamp", packetizer)
        self.assertNotIn("honch_packetizer_device_time_source", packetizer)

    def test_flush_uses_single_parsed_record_scratch(self) -> None:
        internal = read_sdk("core/src/honch_internal.h")
        queue_policy = read_sdk("core/src/honch_queue_policy.c")
        wire = read_sdk("core/src/honch_wire_v2.c")

        self.assertIn("flush_parsed_record", internal)
        self.assertNotIn("flush_parsed_records[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertIn("honch_wire_v2_encode_event_batch_provider", wire)
        self.assertIn("honch_wire_v2_measure_event_batch_provider", wire)
        self.assertIn("honch_flush_wire_event_provider", queue_policy)

    def test_queue_supports_configurable_durability_mode(self) -> None:
        public = read("include/honch/honch.h") + read_sdk("core/include/honch/core/config.h")
        queue = read_sdk("ports/posix/src/posix_storage.c")
        core = read_sdk("core/src/honch_core.c")

        self.assertIn("honch_durability_mode_t", public)
        self.assertIn("durability_mode", public)
        self.assertIn("HONCH_DURABILITY_OS_BUFFERED = 0", public)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS = 1", public)
        self.assertIn("HONCH_DURABILITY_DEFAULT = HONCH_DURABILITY_OS_BUFFERED", public)
        self.assertIn("client->durability_mode", queue)
        self.assertIn("HONCH_DURABILITY_SYNC_ALWAYS ? HONCH_DURABILITY_SYNC_ALWAYS : HONCH_DURABILITY_OS_BUFFERED", core)

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
        root_readme = read_sdk("README.md")
        production_e2e = read_sdk("docs/production-e2e.md")
        release_runner = read_sdk("tools/release_e2e.py")

        self.assertIn("/capture", readme)
        self.assertIn("application/vnd.honch.chunk", readme)
        self.assertIn("POST /capture", spec)
        self.assertNotIn("application/cbor", readme)
        self.assertNotIn("POST /batch", readme)
        self.assertIn("HQR1 queue records", root_readme)
        self.assertIn("ports/posix/test/test_sdk_contract.py", production_e2e)
        self.assertIn("ports/esp-idf/tests/test_sdk_contract.py", production_e2e)
        self.assertIn("ports/posix/test/test_sdk_contract.py", release_runner)
        self.assertIn("ports/esp-idf/tests/test_sdk_contract.py", release_runner)
        self.assertNotIn("test_cbor_migration.py", production_e2e)
        self.assertNotIn("test_cbor_migration.py", release_runner)

    def test_docs_warn_tick_may_block_and_show_dedicated_pump_thread(self) -> None:
        readme = read("README.md")
        normalized = " ".join(readme.split())

        self.assertIn("honch_tick() may block for up to the configured transport timeout", normalized)
        self.assertIn("Do not call honch_tick() from a latency-sensitive control loop", normalized)
        self.assertIn("pthread_create", readme)
        self.assertIn("honch_tick(client);", readme)
        self.assertIn("usleep(250000);", readme)

    def test_generated_ids_fail_closed_when_secure_randomness_is_unavailable(self) -> None:
        platform = read_sdk("ports/posix/src/posix_platform.c")
        random_hex = c_function_body(platform, "honch_random_hex")

        self.assertIn("honch_posix_random_bytes(NULL, bytes, sizeof(bytes))", random_hex)
        self.assertNotIn("getpid()", random_hex)
        self.assertNotIn("honch_now_millis()", random_hex)

    def test_posix_reset_errors_after_identity_snapshot_use_cleanup_path(self) -> None:
        state = read_sdk("ports/posix/src/posix_state.c")
        reset = c_function_body(state, "honch_state_reset")

        snapshot_index = reset.find("previous_distinct_id = honch_strdup")
        cleanup_index = reset.find("cleanup:")
        self.assertGreaterEqual(snapshot_index, 0)
        self.assertGreater(
            cleanup_index,
            snapshot_index,
            "reset must have a shared cleanup path after previous identities are copied",
        )

        post_snapshot = reset[snapshot_index:cleanup_index]
        early_returns = [
            line.strip()
            for line in post_snapshot.splitlines()
            if line.strip().startswith("return ")
        ]
        self.assertEqual(
            [],
            early_returns,
            "post-snapshot reset failures must not return before freeing previous identities",
        )

    def test_wire_v2_empty_byte_append_skips_memcpy(self) -> None:
        wire = read_sdk("core/src/honch_wire_v2.c")
        append_bytes = c_function_body(wire, "honch_wire_v2_append_bytes")

        zero_guard_index = append_bytes.find("if (value_size == 0u)")
        memcpy_index = append_bytes.find("memcpy(")
        self.assertGreaterEqual(zero_guard_index, 0)
        self.assertGreater(memcpy_index, zero_guard_index)


if __name__ == "__main__":
    unittest.main(verbosity=2)
