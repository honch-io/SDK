#!/usr/bin/env python3
"""Static checks for the ESP-IDF default chunk wire transport."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


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


def cmake_list_values(source: str, variable: str) -> set[str]:
    values: set[str] = set()
    patterns = (
        rf"set\(\s*{re.escape(variable)}\s+(.*?)\)",
        rf"list\(\s*APPEND\s+{re.escape(variable)}\s+(.*?)\)",
    )
    for pattern in patterns:
        for match in re.finditer(pattern, source, re.DOTALL):
            body = re.sub(r"#.*", "", match.group(1))
            for token in re.split(r"\s+", body.strip()):
                if token:
                    values.add(token.strip('"'))
    return values


class EspIdfChunkWireTest(unittest.TestCase):
    def test_esp_idf_component_uses_repo_root_package_layout(self) -> None:
        root_cmake = read("CMakeLists.txt")
        root_manifest = read("idf_component.yml")
        component_cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        example_cmake = read("ports/esp-idf/example/CMakeLists.txt")
        bench_cmake = read("ports/esp-idf/benchtest/CMakeLists.txt")
        rate_sweep_cmake = read("ports/esp-idf/rate_sweep_bench/CMakeLists.txt")
        footprint_cmake = read("ports/esp-idf/footprint/CMakeLists.txt")

        self.assertIn("if(ESP_PLATFORM)", root_cmake)
        self.assertIn("ports/esp-idf/honch/CMakeLists.txt", root_cmake)
        self.assertIn("description: \"Honch product analytics SDK for ESP-IDF\"", root_manifest)
        self.assertIn("HONCH_SDK_ROOT", component_cmake)
        self.assertIn("core/src/honch_core.c", component_cmake)
        self.assertIn("HONCH_FLUSH_TIMING", component_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../../..")', example_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', bench_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', rate_sweep_cmake)
        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', footprint_cmake)

    def test_esp_idf_examples_include_optional_local_defaults(self) -> None:
        for path in [
            "ports/esp-idf/example/CMakeLists.txt",
            "ports/esp-idf/example_gpio/CMakeLists.txt",
            "ports/esp-idf/benchtest/CMakeLists.txt",
            "ports/esp-idf/rate_sweep_bench/CMakeLists.txt",
        ]:
            cmake = read(path)
            self.assertIn("SDKCONFIG_DEFAULTS", cmake, path)
            self.assertIn("local/sdkconfig.defaults", cmake, path)

    def test_esp_idf_local_defaults_template_is_safe_to_commit(self) -> None:
        template = read("ports/esp-idf/local/sdkconfig.defaults.example")

        self.assertIn('CONFIG_WIFI_SSID="your-ssid"', template)
        self.assertIn('CONFIG_WIFI_PASSWORD="your-password"', template)
        self.assertIn('CONFIG_HONCH_API_KEY="honch_e2e_test_key"', template)
        self.assertIn('CONFIG_HONCH_HOST="http://192.168.1.122:8001"', template)

    def test_esp_idf_example_has_identify_pilot_sequence(self) -> None:
        example = read("ports/esp-idf/example/main/app_main.c")
        sequence = c_function_body(example, "run_identify_pilot")

        self.assertIn("esp32_identify_pre_", sequence)
        self.assertIn("esp32-identify-user-", sequence)
        self.assertIn("honch_identify(user_id", sequence)
        self.assertIn("esp32_identify_post_", sequence)
        self.assertIn("honch_flush()", sequence)
        self.assertIn("IDENTIFY_PILOT run_id=%s device_id=%s user_id=%s", sequence)
        self.assertIn("HONCH_IDENTIFY_PILOT_TASK_STACK_BYTES", example)
        self.assertIn('"honch_identify_pilot"', example)
        self.assertIn("xTaskCreate(honch_identify_pilot_task", example)
        self.assertIn(".flush_max_batches = 8", example)
        self.assertIn(".shutdown_flush_max_batches = 8", example)
        self.assertLess(sequence.find("honch_err_t pre_status = honch_track"), sequence.find("honch_identify(user_id"))
        self.assertLess(sequence.find("honch_identify(user_id"), sequence.find("honch_err_t post_status = honch_track"))
        self.assertLess(sequence.find("honch_err_t post_status = honch_track"), sequence.find("honch_flush()"))

    def test_esp_transport_ops_post_chunk_wire_to_capture_endpoint(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        post_chunk = c_function_body(transport, "honch_esp_post_chunk")
        ops_init = c_function_body(transport, "honch_esp_transport_ops_init")

        self.assertIn('"src/esp_transport_http.c"', cmake)
        self.assertIn("#include \"honch/core/transport.h\"", adapter)
        self.assertIn("esp_http_client_init", transport)
        self.assertIn("esp_http_client_handle_t http_client", adapter)
        self.assertIn("char *capture_url", adapter)
        self.assertIn("keep_alive_enable = true", transport)
        self.assertNotIn("#include \"esp_event.h\"", transport)
        self.assertNotIn("#include \"esp_netif.h\"", transport)
        self.assertNotIn("honch_esp_ensure_transport_ready", transport)
        self.assertNotIn("esp_event_loop_create_default()", transport)
        self.assertNotIn("esp_netif_init()", transport)
        self.assertNotIn("esp_event_loop_create_default()", ops_init)
        self.assertNotIn("esp_netif_init()", ops_init)
        self.assertNotIn("esp_event_loop_create_default()", post_chunk)
        self.assertNotIn("esp_netif_init()", post_chunk)
        self.assertIn('"/capture"', transport)
        self.assertIn('"Content-Type", "application/vnd.honch.chunk"', transport)
        self.assertIn('"X-Honch-Project-Key"', transport)
        self.assertIn('"X-Honch-Stream-Id"', transport)
        self.assertIn("HONCH_TRANSPORT_CHUNK_STORED", transport)
        self.assertIn("HONCH_HTTP_TIMING", transport)
        self.assertIn("reused_client", transport)
        self.assertIn("client_reset", transport)
        self.assertIn(".post_chunk = honch_esp_post_chunk", transport)
        self.assertIn(".ctx = ctx", transport)
        self.assertNotIn(".ctx = NULL", transport)
        self.assertNotIn("post_batch", transport)
        self.assertNotIn("application/cbor", transport)
        self.assertNotIn("Content-Encoding", transport)
        self.assertNotIn("tdefl_compress_mem_to_heap", transport)

    def test_esp_transport_reuses_http_client_and_deinitializes_it(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")

        prepare_client = c_function_body(transport, "honch_esp_transport_prepare_client")
        reset_client = c_function_body(transport, "honch_esp_transport_reset_http_client")
        post_chunk = c_function_body(transport, "honch_esp_post_chunk")
        deinit = c_function_body(transport, "honch_esp_transport_ops_deinit")
        shutdown = c_function_body(compat, "honch_shutdown")

        self.assertIn("honch_esp_transport_ops_deinit", adapter)
        self.assertIn("if (transport->http_client != NULL && transport->timeout_ms == timeout_ms)", prepare_client)
        self.assertIn("*reused_client = true;", prepare_client)
        self.assertIn("honch_esp_transport_reset_http_client(transport);", prepare_client)
        self.assertIn("esp_http_client_cleanup(transport->http_client);", reset_client)
        self.assertIn("transport->http_client = NULL;", reset_client)
        self.assertIn("esp_http_client_set_post_field(http_client, NULL, 0)", post_chunk)
        self.assertNotIn("esp_http_client_cleanup(http_client);", post_chunk)
        self.assertIn("honch_esp_transport_reset_http_client(ctx);", deinit)
        self.assertIn("honch_esp_transport_clear_urls(ctx);", deinit)
        self.assertIn("honch_esp_transport_ops_deinit(&s_transport_ctx);", shutdown)
        self.assertLess(
            shutdown.find("honch_core_shutdown(client)"),
            shutdown.find("honch_esp_transport_ops_deinit(&s_transport_ctx);"),
        )
        busy_index = shutdown.find("if (status == HONCH_STATUS_ERROR_BUSY)")
        busy_return_index = shutdown.find("return HONCH_ERR_BUSY;", busy_index)
        deinit_index = shutdown.find("honch_esp_transport_ops_deinit(&s_transport_ctx);")
        self.assertLess(busy_index, busy_return_index)
        self.assertLess(busy_return_index, deinit_index)

    def test_esp_transport_honors_retry_after_on_429(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        core_transport = read("core/include/honch/core/transport.h")
        event_handler = c_function_body(transport, "honch_esp_http_event_handler")
        parse_seconds = c_function_body(transport, "honch_esp_parse_retry_after_seconds")
        parse_date = c_function_body(transport, "honch_esp_parse_retry_after_http_date")
        post_chunk = c_function_body(transport, "honch_esp_post_chunk")
        ops_init = c_function_body(transport, "honch_esp_transport_ops_init")

        self.assertIn("uint64_t (*retry_after_ms)(void *ctx)", core_transport)
        self.assertIn("uint64_t retry_after_ms", adapter)
        self.assertIn("honch_esp_parse_retry_after_seconds", transport)
        self.assertIn("honch_esp_parse_retry_after_http_date", transport)
        self.assertIn("*delay_ms = seconds", parse_seconds)
        self.assertIn("* 1000u", parse_seconds)
        self.assertIn("%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d %3[A-Za-z]%n", parse_date)
        self.assertIn("strcasecmp(gmt, \"GMT\")", parse_date)
        self.assertIn("target_ms - now_ms", parse_date)
        self.assertIn("HTTP_EVENT_ON_HEADER", event_handler)
        self.assertIn('"Retry-After"', event_handler)
        self.assertIn("honch_esp_parse_retry_after", event_handler)
        self.assertIn("transport->retry_after_ms = 0u", post_chunk)
        self.assertIn("status == 429", post_chunk)
        self.assertIn(".retry_after_ms = honch_esp_retry_after_ms", ops_init)

    def test_esp_compat_layer_delegates_public_api_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        shims = read("ports/esp-idf/honch/src/esp_core_shims.c")

        self.assertIn('"src/esp_compat.c"', cmake)
        self.assertIn('"src/esp_core_shims.c"', cmake)
        self.assertIn("#define HONCH_CORE_NO_SHORT_STATUS_NAMES", compat)
        self.assertIn("#include \"honch/core/honch.h\"", compat)
        self.assertIn("static honch_client_t *s_client", compat)
        self.assertIn("honch_esp_platform_ops_init(&platform_ops", compat)
        self.assertIn("honch_esp_event_queue_ops_init(", compat)
        self.assertIn("config->event_queue_ops", compat)
        self.assertIn("honch_esp_transport_ops_init(&transport_ops", compat)
        self.assertIn("honch_esp_transport_ops_deinit(&s_transport_ctx)", compat)
        self.assertIn("honch_core_init(&next", compat)
        self.assertIn("honch_esp_client_acquire(&client)", compat)
        self.assertIn("honch_core_track(client", compat)
        self.assertIn("honch_core_report_error(client", compat)
        self.assertIn("honch_core_flush(client)", compat)
        self.assertNotIn("honch_core_track(s_client", compat)
        self.assertNotIn("honch_core_report_error(s_client", compat)
        self.assertNotIn("honch_core_flush(s_client)", compat)
        self.assertIn("honch_client_t", adapter)
        self.assertIn("honch_state_prepare", shims)

    def test_esp_idf_passes_reset_reason_snapshot_to_core(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        public = read("ports/esp-idf/honch/include/honch.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn("bool enable_error_tracking;", public)
        self.assertIn("bool enable_crash_symbolication;", public)
        self.assertIn("honch_fault_snapshot_t honch_esp_fault_snapshot(bool include_symbolication_context);", adapter)
        self.assertIn("#include \"esp_system.h\"", platform)
        self.assertIn("esp_reset_reason()", platform)
        self.assertIn("ESP_RST_PANIC", platform)
        self.assertIn("ESP_RST_TASK_WDT", platform)
        self.assertIn("ESP_RST_INT_WDT", platform)
        self.assertIn("ESP_RST_BROWNOUT", platform)
        self.assertIn("HONCH_FAULT_KIND_PANIC", platform)
        self.assertIn("HONCH_FAULT_KIND_WATCHDOG", platform)
        self.assertIn("HONCH_FAULT_KIND_BROWNOUT", platform)
        self.assertIn("honch_esp_firmware_build_id", platform)
        self.assertIn("esp_app_get_elf_sha256(build_id, sizeof(build_id))", platform)
        self.assertNotIn("app_elf_sha256[length]", platform)
        self.assertIn("include_symbolication_context", platform)
        self.assertIn("esp_core_dump_get_summary", platform)
        self.assertIn("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH", platform)
        self.assertIn("CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF", platform)
        self.assertIn("crash_summary_version = has_crash_summary ? 1u : 0u", platform)
        self.assertIn("firmware_build_id", platform)
        self.assertIn("fault_pc", platform)
        self.assertIn("backtrace", platform)
        self.assertIn("espcoredump", cmake)
        self.assertNotIn("esp_panic_handler", platform)
        self.assertIn("core_config.enable_error_tracking = should_emit_fault_snapshot;", compat)
        self.assertIn("should_emit_fault_snapshot && config->enable_crash_symbolication", compat)
        self.assertIn("core_config.fault_snapshot = &fault_snapshot;", compat)

    def test_esp_public_config_appends_error_tracking_fields(self) -> None:
        public = read("ports/esp-idf/honch/include/honch.h")

        self.assertLess(
            public.index("const honch_state_storage_ops_t *state_storage_ops;"),
            public.index("const honch_event_queue_ops_t *event_queue_ops;"),
        )
        self.assertLess(
            public.index("const honch_event_queue_ops_t *event_queue_ops;"),
            public.index("bool enable_error_tracking;"),
        )
        self.assertLess(
            public.index("bool enable_error_tracking;"),
            public.index("bool enable_crash_symbolication;"),
        )

    def test_esp_idf_consumes_boot_fault_snapshot_once_per_boot(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        init_body = c_function_body(compat, "honch_init")

        self.assertIn("static bool s_fault_snapshot_consumed = false;", compat)
        self.assertIn("bool should_emit_fault_snapshot =", init_body)
        self.assertIn("config->enable_error_tracking && !s_fault_snapshot_consumed", init_body)
        self.assertIn("core_config.enable_error_tracking = should_emit_fault_snapshot;", init_body)
        self.assertIn("should_emit_fault_snapshot && config->enable_crash_symbolication", init_body)
        self.assertIn("if (should_emit_fault_snapshot) {\n        s_fault_snapshot_consumed = true;\n    }", init_body)

    def test_esp_idf_maps_idf_5_1_reset_reasons_without_fatal_unknown(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        snapshot_body = c_function_body(platform, "honch_esp_fault_snapshot")

        for reset_reason, reset_string in (
            ("ESP_RST_USB", '"usb"'),
            ("ESP_RST_JTAG", '"jtag"'),
            ("ESP_RST_EFUSE", '"efuse"'),
        ):
            self.assertIn(reset_reason, snapshot_body)
            self.assertIn(reset_string, snapshot_body)

        self.assertIn("ESP_RST_PWR_GLITCH", snapshot_body)
        self.assertIn('"power_glitch"', snapshot_body)
        self.assertIn("HONCH_FAULT_KIND_BROWNOUT", snapshot_body)
        self.assertIn("ESP_IDF_VERSION_VAL(5, 1, 0)", platform)

    def test_esp_idf_attaches_coredump_summary_only_for_panic_reset(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        snapshot_body = c_function_body(platform, "honch_esp_fault_snapshot")

        self.assertIn(
            'honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_PANIC, "panic", include_symbolication_context)',
            snapshot_body,
        )
        for reset_reason in (
            '"interrupt_wdt"',
            '"task_wdt"',
            '"watchdog"',
            '"brownout"',
            '"power_glitch"',
        ):
            self.assertIn(reset_reason, snapshot_body)
            self.assertNotIn(f"{reset_reason}, include_symbolication_context", snapshot_body)
        self.assertIn("honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, \"interrupt_wdt\", false)", snapshot_body)
        self.assertIn("honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, \"task_wdt\", false)", snapshot_body)
        self.assertIn("honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_WATCHDOG, \"watchdog\", false)", snapshot_body)
        self.assertIn("honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_BROWNOUT, \"brownout\", false)", snapshot_body)
        self.assertIn("HONCH_FAULT_KIND_BROWNOUT,\n                \"power_glitch\",\n                false", snapshot_body)
        self.assertIn("honch_esp_abnormal_fault_snapshot(HONCH_FAULT_KIND_UNKNOWN, \"unknown\", false)", snapshot_body)

    def test_esp_idf_claims_crash_summary_only_when_summary_read_succeeds(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        fill_body = c_function_body(platform, "honch_esp_crash_summary_fill")
        abnormal_body = c_function_body(platform, "honch_esp_abnormal_fault_snapshot")

        self.assertIn("static bool honch_esp_crash_summary_fill", platform)
        self.assertIn("return false;", fill_body)
        self.assertIn("return true;", fill_body)
        self.assertIn(
            "bool has_crash_summary = include_symbolication_context && honch_esp_crash_summary_fill(&summary);",
            abnormal_body,
        )
        self.assertIn("build_id = has_crash_summary ? honch_esp_firmware_build_id() : NULL;", abnormal_body)
        self.assertIn(".crash_summary_version = has_crash_summary ? 1u : 0u", abnormal_body)

    def test_esp_idf_omits_corrupted_xtensa_backtraces(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        fill_body = c_function_body(platform, "honch_esp_crash_summary_fill")
        corrupted_check = "if (!core_summary.exc_bt_info.corrupted)"
        loop_start = "for (uint32_t i = 0u; i < depth; i++)"

        self.assertIn(corrupted_check, fill_body)
        self.assertLess(fill_body.find(corrupted_check), fill_body.find(loop_start))

    def test_esp_idf_error_tracking_is_build_strip_modular(self) -> None:
        root_kconfig = read("Kconfig")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        kconfig = read("ports/esp-idf/honch/Kconfig")
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")

        self.assertIn('rsource "ports/esp-idf/honch/Kconfig"', root_kconfig)
        self.assertIn("config HONCH_ERROR_TRACKING", kconfig)
        self.assertIn("config HONCH_CRASH_SYMBOLICATION", kconfig)
        self.assertRegex(kconfig, r"config HONCH_ERROR_TRACKING\s+bool[^\n]*\n\s+default y")
        self.assertRegex(kconfig, r"config HONCH_CRASH_SYMBOLICATION\s+bool[^\n]*\n\s+depends on HONCH_ERROR_TRACKING\s+default y")
        self.assertIn("depends on HONCH_ERROR_TRACKING", kconfig)
        self.assertIn("CONFIG_HONCH_ERROR_TRACKING", cmake)
        self.assertIn("CONFIG_HONCH_CRASH_SYMBOLICATION", cmake)
        self.assertIn("HONCH_ENABLE_ERROR_TRACKING=0", cmake)
        self.assertIn("HONCH_ENABLE_CRASH_SYMBOLICATION=0", cmake)
        self.assertIn("if(CONFIG_HONCH_CRASH_SYMBOLICATION)", cmake)
        self.assertIn("list(APPEND HONCH_ESP_REQUIRES espcoredump)", cmake)
        self.assertNotIn("espcoredump\n        esp_http_client", cmake)
        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", compat)
        self.assertIn("#if HONCH_ENABLE_ERROR_TRACKING", adapter)

    def test_esp_default_storage_is_ram_queue_only(self) -> None:
        storage = read("ports/esp-idf/honch/src/esp_storage.c")
        adapter = read("ports/esp-idf/honch/src/esp_core_adapter.h")
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")

        self.assertIn('"src/esp_storage.c"', cmake)
        self.assertIn('"core/src/honch_ram_queue.c"', cmake)
        self.assertIn("#include \"honch/core/storage.h\"", adapter)
        self.assertIn("#include \"honch/core/ram_queue.h\"", adapter)
        self.assertIn("esp_app_format", cmake)
        self.assertIn("honch_ram_queue_init", storage)
        self.assertIn("honch_ram_queue_ops_init", storage)
        self.assertNotIn("nvs_", storage.lower())
        self.assertNotIn("nvs_flash", cmake)
        self.assertNotIn("esp_storage_nvs", cmake)

    def test_public_config_has_no_legacy_wire_toggles(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")

        combined = compat + public_header + readme
        self.assertNotIn("enable_wire_v2", combined)
        self.assertNotIn("disable_gzip", combined)
        self.assertNotIn("gzip_min_bytes", combined)

    def test_readme_tracks_typed_properties_not_json_strings(self) -> None:
        readme = read("ports/esp-idf/README.md")

        self.assertIn("honch_property_t", readme)
        self.assertIn("honch_track(\"button_pressed\", button_props, 1u)", readme)
        self.assertNotIn("honch_track(\"button_pressed\", \"{", readme)
        self.assertNotIn("honch_track(\"screen_viewed\", \"{", readme)

    def test_esp_idf_uses_cooperative_tick_config(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")
        internal = read("core/src/honch_internal.h")
        example = read("ports/esp-idf/example/main/app_main.c")
        gpio_example = read("ports/esp-idf/example_gpio/main/app_main.c")
        app_main = c_function_body(example, "app_main")
        honch_task = c_function_body(example, "honch_telemetry_task")

        self.assertIn("#define HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS 3000u", internal)
        self.assertIn("flush_interval_seconds", public_header)
        self.assertIn("flush_event_threshold", public_header)
        self.assertIn("uint32_t transport_timeout_ms", public_header)
        self.assertIn("honch_tick", public_header)
        self.assertIn("honch_core_tick(client)", compat)
        self.assertIn("if (config->flush_event_threshold > 0u)", compat)
        self.assertIn(
            "core_config.batch_size = config->flush_event_threshold > HONCH_MAX_BATCH_SIZE",
            compat,
        )
        self.assertIn("core_config.transport_timeout_ms = config->transport_timeout_ms", compat)
        self.assertIn("core_config.flush_interval_seconds = config->flush_interval_seconds", compat)
        self.assertIn("core_config.flush_event_threshold = config->flush_event_threshold", compat)
        self.assertIn("xTaskCreate(honch_telemetry_task", example)
        self.assertIn("#define HONCH_TELEMETRY_TASK_STACK_BYTES 8192", example)
        self.assertIn("HONCH_TELEMETRY_TASK_STACK_BYTES", app_main)
        self.assertNotIn("HONCH_TELEMETRY_TASK_STACK_WORDS", example)
        self.assertIn("#define HONCH_TELEMETRY_TASK_STACK_BYTES 8192", gpio_example)
        self.assertIn("HONCH_TELEMETRY_TASK_STACK_BYTES", gpio_example)
        self.assertNotIn("HONCH_TELEMETRY_TASK_STACK_WORDS", gpio_example)
        self.assertIn("#define HONCH_TELEMETRY_TASK_STACK_BYTES 8192", readme)
        self.assertIn("HONCH_TELEMETRY_TASK_STACK_BYTES", readme)
        self.assertIn("ESP-IDF `xTaskCreate()` stack sizes are in bytes", readme)
        self.assertIn("at least 8192 bytes", readme)
        self.assertIn("honch_tick();", honch_task)
        self.assertNotIn("honch_tick();", app_main)
        self.assertIn("low-priority telemetry task", readme)
        self.assertIn("can perform blocking network I/O", readme)
        self.assertIn("SDK-owned worker task", readme)
        self.assertIn("application-owned FreeRTOS task", readme)
        self.assertIn("transport_timeout_ms", readme)
        self.assertIn("3000", readme)
        self.assertIn("maximum of 10000 ms", readme)
        self.assertNotIn("disable_background_flush", compat + public_header + readme)

    def test_esp_transport_timeout_is_hard_capped(self) -> None:
        transport = read("ports/esp-idf/honch/src/esp_transport_http.c")
        ops_init = c_function_body(transport, "honch_esp_transport_ops_init")

        self.assertIn("#define HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS 10000u", transport)
        self.assertIn("if (effective_timeout_ms > HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS)", ops_init)
        self.assertIn("effective_timeout_ms = HONCH_ESP_MAX_TRANSPORT_TIMEOUT_MS;", ops_init)
        self.assertIn(".timeout_ms = timeout_ms", transport)

    def test_esp_static_config_string_limits_are_public_constants(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")

        self.assertIn("#define HONCH_ESP_API_KEY_MAX_LENGTH 127u", public_header)
        self.assertIn("#define HONCH_ESP_ENDPOINT_URL_MAX_LENGTH 279u", public_header)
        self.assertIn("#define HONCH_ESP_DEVICE_MODEL_MAX_LENGTH 63u", public_header)
        self.assertIn("#define HONCH_ESP_FIRMWARE_VERSION_MAX_LENGTH 31u", public_header)
        self.assertIn("#define HONCH_ESP_ENVIRONMENT_MAX_LENGTH 31u", public_header)
        self.assertIn("s_api_key[HONCH_ESP_API_KEY_MAX_LENGTH + 1u]", compat)
        self.assertIn("s_endpoint_url[HONCH_ESP_ENDPOINT_URL_MAX_LENGTH + 1u]", compat)
        self.assertIn("s_device_model[HONCH_ESP_DEVICE_MODEL_MAX_LENGTH + 1u]", compat)
        self.assertIn("s_firmware_version[HONCH_ESP_FIRMWARE_VERSION_MAX_LENGTH + 1u]", compat)
        self.assertIn("s_environment[HONCH_ESP_ENVIRONMENT_MAX_LENGTH + 1u]", compat)

    def test_example_initializes_network_stack_for_offline_tick_smoke(self) -> None:
        example = read("ports/esp-idf/example/main/app_main.c")
        network_stack = c_function_body(example, "init_network_stack")
        wifi_init = c_function_body(example, "wifi_init_sta")
        app_main = c_function_body(example, "app_main")

        self.assertIn("ESP_ERROR_CHECK(esp_netif_init());", network_stack)
        self.assertIn("ESP_ERROR_CHECK(esp_event_loop_create_default());", network_stack)
        self.assertIn("init_network_stack();", wifi_init)
        self.assertIn("init_network_stack();", app_main)
        self.assertLess(app_main.find("init_network_stack();"), app_main.find("honch_init(&config);"))

    def test_esp_idf_flush_spacing_is_configurable(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")
        internal = read("core/src/honch_internal.h")
        config = read("core/include/honch/core/config.h")

        self.assertIn("#define HONCH_DEFAULT_FLUSH_MIN_INTERVAL_MS 10000u", internal)
        self.assertIn("unsigned int flush_min_interval_ms", config)
        self.assertIn("uint32_t flush_min_interval_ms", public_header)
        self.assertIn("core_config.flush_min_interval_ms = config->flush_min_interval_ms", compat)
        self.assertIn("flush_min_interval_ms", readme)
        self.assertIn("10000", readme)
        self.assertIn("HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", readme)

    def test_esp_idf_connectivity_gate_is_configurable(self) -> None:
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")
        config = read("core/include/honch/core/config.h")
        core = read("core/src/honch_core.c")

        self.assertIn("honch_connectivity_fn connectivity_callback", config)
        self.assertIn("void *connectivity_userdata", config)
        self.assertIn("bool (*connectivity_callback)(void)", public_header)
        self.assertIn("core_config.connectivity_callback = honch_esp_connectivity_callback", compat)
        self.assertIn("core_config.connectivity_userdata = NULL", compat)
        self.assertIn("HONCH_STATUS_ERROR_OFFLINE", compat)
        self.assertIn("honch_scheduler_connectivity_ready_locked", core)
        self.assertIn("connectivity_callback", readme)
        self.assertIn("Do not call `honch_tick()` while connectivity is unavailable", readme)

    def test_esp_idf_benchmarks_disable_flush_spacing(self) -> None:
        benchtest = read("ports/esp-idf/benchtest/main/app_main.c")
        rate_sweep = read("ports/esp-idf/rate_sweep_bench/main/app_main.c")

        self.assertIn(".flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", benchtest)
        self.assertIn(".flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", rate_sweep)

    def test_core_flush_and_shutdown_drains_are_bounded(self) -> None:
        core = read("core/src/honch_core.c")
        internal = read("core/src/honch_internal.h")
        config = read("core/include/honch/core/config.h")
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        public_header = read("ports/esp-idf/honch/include/honch.h")
        readme = read("ports/esp-idf/README.md")

        core_flush = c_function_body(core, "honch_core_flush")
        core_tick = c_function_body(core, "honch_core_tick")
        shutdown = c_function_body(core, "honch_core_shutdown")

        self.assertIn("#define HONCH_DEFAULT_FLUSH_MAX_BATCHES 1u", internal)
        self.assertIn("#define HONCH_DEFAULT_SHUTDOWN_FLUSH_MAX_BATCHES 1u", internal)
        self.assertIn("size_t flush_max_batches", config)
        self.assertIn("size_t shutdown_flush_max_batches", config)
        self.assertIn("size_t flush_max_batches", internal)
        self.assertIn("size_t shutdown_flush_max_batches", internal)
        self.assertIn("honch_queue_flush_limited_locked(client, client->flush_max_batches)", core_flush)
        self.assertIn(
            "honch_queue_flush_limited_locked(client, client->shutdown_flush_max_batches)",
            shutdown,
        )
        self.assertIn("honch_queue_flush_one_chunk_locked(client, &progressed)", core_tick)
        self.assertNotIn("honch_queue_flush_limited_locked", core_tick)
        self.assertIn("uint32_t flush_max_batches", public_header)
        self.assertIn("uint32_t shutdown_flush_max_batches", public_header)
        self.assertIn("core_config.flush_max_batches = config->flush_max_batches", compat)
        self.assertIn("core_config.shutdown_flush_max_batches = config->shutdown_flush_max_batches", compat)
        self.assertIn("flush_max_batches", readme)
        self.assertIn("shutdown_flush_max_batches", readme)
        self.assertIn("bounded", readme)
        self.assertIn("may remain queued", readme)

    def test_esp_idf_sdk_lock_waits_are_bounded_and_fail_open(self) -> None:
        platform = read("ports/esp-idf/honch/src/esp_platform.c")
        compat = read("ports/esp-idf/honch/src/esp_compat.c")
        component_cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        combined = platform + compat + component_cmake

        self.assertNotIn("portMAX_DELAY", combined)

        platform_lock = c_function_body(platform, "honch_esp_mutex_lock")
        self.assertIn("HONCH_ESP_MUTEX_LOCK_TIMEOUT_MS", platform)
        self.assertIn("honch_esp_lock_ticks(HONCH_ESP_MUTEX_LOCK_TIMEOUT_MS)", platform_lock)
        self.assertIn("HONCH_STATUS_ERROR_BUSY", platform_lock)

        client_lock = c_function_body(compat, "honch_esp_client_lock")
        client_acquire = c_function_body(compat, "honch_esp_client_acquire")
        client_detach = c_function_body(compat, "honch_esp_client_detach")
        init_begin = c_function_body(compat, "honch_esp_init_begin")
        self.assertIn("static honch_err_t honch_esp_client_lock(void)", compat)
        self.assertIn("HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS", compat)
        self.assertIn("honch_esp_lock_ticks(HONCH_ESP_CLIENT_LOCK_TIMEOUT_MS)", client_lock)
        self.assertIn("return HONCH_ERR_BUSY;", client_lock)
        self.assertIn("honch_err_t err = honch_esp_client_lock();", client_acquire)
        self.assertIn("return err;", client_acquire)
        self.assertIn("honch_err_t err = honch_esp_client_lock();", client_detach)
        self.assertIn("return err;", client_detach)
        self.assertIn("honch_err_t err = honch_esp_client_lock();", init_begin)
        self.assertIn("return err;", init_begin)

        self.assertNotIn("honch_esp_gpio_shutdown_hook", compat)
        self.assertNotIn("esp_gpio_adapter", component_cmake)
        self.assertNotIn("esp_gpio_public", component_cmake)

    def test_auto_property_collection_uses_client_owned_buffers(self) -> None:
        core = read("core/src/honch_core.c")
        internal = read("core/src/honch_internal.h")
        collect = c_function_body(core, "honch_collect_auto_properties")
        acquire = c_function_body(core, "honch_acquire_auto_property_buffer")
        release = c_function_body(core, "honch_auto_properties_snapshot_free")

        self.assertIn("auto_property_buffers", internal)
        self.assertIn("auto_property_buffer_in_use", internal)
        self.assertIn("#define HONCH_AUTO_PROPERTY_BUFFER_COUNT 2u", internal)
        self.assertIn("honch_acquire_auto_property_buffer", core)
        self.assertIn("HONCH_ERROR_BUSY", acquire)
        self.assertNotIn("calloc", collect)
        self.assertNotIn("malloc", collect)
        self.assertNotIn("free(", release)

    def test_flush_uses_client_owned_scratch_buffers(self) -> None:
        core = read("core/src/honch_queue_policy.c")
        internal = read("core/src/honch_internal.h")
        read_batch = c_function_body(core, "honch_core_read_queue_batch")
        build_message = c_function_body(core, "honch_core_build_wire_v2_message")
        post_message = c_function_body(core, "honch_core_post_wire_v2_message_limited")
        flush_one = c_function_body(core, "honch_queue_flush_one_with_chunk_limit_locked")
        core_flush = c_function_body(read("core/src/honch_core.c"), "honch_core_flush")

        self.assertIn("#define HONCH_DEFAULT_FLUSH_SCRATCH_MAX_EVENTS 4u", internal)
        self.assertIn("#define HONCH_FLUSH_SCRATCH_MAX_EVENTS HONCH_DEFAULT_FLUSH_SCRATCH_MAX_EVENTS", internal)
        self.assertIn("flush_events[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertIn("flush_sequences[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertIn("flush_storage_events[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertIn("flush_parsed_record", internal)
        self.assertNotIn("flush_parsed_records[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertNotIn("flush_compact_events[HONCH_FLUSH_SCRATCH_MAX_EVENTS]", internal)
        self.assertIn("flush_message_buffer[HONCH_WIRE_V2_MAX_FRAME_BYTES]", internal)
        self.assertIn("flush_frame_buffer[HONCH_WIRE_V2_MAX_FRAME_BYTES]", internal)
        self.assertIn("batch_size > HONCH_FLUSH_SCRATCH_MAX_EVENTS", flush_one)
        self.assertIn("client->flush_in_progress", core_flush)
        self.assertNotIn("calloc", read_batch)
        self.assertNotIn("calloc", build_message)
        self.assertNotIn("malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES)", build_message)
        self.assertNotIn("malloc(HONCH_WIRE_V2_MAX_FRAME_BYTES)", post_message)
        self.assertNotIn("calloc(batch_size", flush_one)

    def test_readme_only_documents_implemented_automatic_esp_properties(self) -> None:
        readme = read("ports/esp-idf/README.md")
        automatic = readme.split("## What gets sent automatically", 1)[1].split("## Configuration options", 1)[0]

        self.assertIn("$device_id` — from config or derived from the ESP MAC address", automatic)
        self.assertIn("derived from the ESP MAC address", automatic)
        self.assertNotIn("$wifi_rssi", automatic)
        self.assertIn("$device_boot` — on init, with `reset_reason` property", automatic)
        self.assertNotIn("$connectivity_change", automatic)

    def test_esp_package_metadata_matches_runtime_sdk_version(self) -> None:
        internal = read("core/src/honch_internal.h")
        root_manifest = read("idf_component.yml")
        component_manifest = read("ports/esp-idf/honch/idf_component.yml")
        readme = read("ports/esp-idf/README.md")

        self.assertIn('#define HONCH_SDK_VERSION "0.2.0"', internal)
        self.assertIn('version: "0.2.0"', root_manifest)
        self.assertIn('version: "0.2.0"', component_manifest)
        self.assertIn('idf.py add-dependency "honch-io/honch^0.2.0"', readme)

    def test_esp_component_dependencies_match_port_sources(self) -> None:
        cmake = read("ports/esp-idf/honch/CMakeLists.txt")
        root_manifest = read("idf_component.yml")
        component_manifest = read("ports/esp-idf/honch/idf_component.yml")

        component_dependencies = cmake_list_values(cmake, "HONCH_ESP_REQUIRES")

        for dependency in (
            "esp_app_format",
            "esp_http_client",
            "esp-tls",
            "esp_timer",
            "efuse",
            "freertos",
            "espcoredump",
        ):
            self.assertIn(dependency, component_dependencies)

        for unused_dependency in (
            "esp_wifi",
            "esp_event",
            "esp_netif",
            "esp_driver_gpio",
            "driver",
            "cbor",
            "espressif__cjson",
        ):
            self.assertNotIn(unused_dependency, component_dependencies)

        for manifest in (root_manifest, component_manifest):
            self.assertNotIn("espressif/cbor", manifest)
            self.assertNotIn("espressif/cjson", manifest)

    def test_ci_triggers_when_shared_core_changes(self) -> None:
        esp_workflow = read(".github/workflows/esp-idf.yml")
        micropython_workflow = read(".github/workflows/micropython.yml")

        self.assertIn("'core/**'", esp_workflow)
        self.assertIn("'core/**'", micropython_workflow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
