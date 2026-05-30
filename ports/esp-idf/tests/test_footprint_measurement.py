import json
import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "tools" / "measure_esp_idf_footprint.py"
spec = importlib.util.spec_from_file_location("measure_esp_idf_footprint", MODULE_PATH)
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)

build_report = measure.build_report
parse_archive_sizes = measure.parse_archive_sizes
parse_monitor_log = measure.parse_monitor_log
parse_total_sizes = measure.parse_total_sizes


TOTAL_BARE = {
    "version": "1.2",
    "total_size": 100000,
    "layout": [
        {"name": "Flash Code", "used": 50000, "parts": {".text": {"size": 50000}}},
        {"name": "Flash Data", "used": 10000, "parts": {".rodata": {"size": 10000}}},
        {"name": "IRAM", "used": 7000, "parts": {".text": {"size": 7000}}},
        {"name": "DRAM", "used": 8000, "parts": {".bss": {"size": 6000}, ".data": {"size": 2000}}},
        {"name": "RTC SLOW", "used": 64, "parts": {".force_slow": {"size": 40}}},
    ],
}

TOTAL_HONCH = {
    "version": "1.2",
    "total_size": 113500,
    "layout": [
        {"name": "Flash Code", "used": 61000, "parts": {".text": {"size": 61000}}},
        {"name": "Flash Data", "used": 12000, "parts": {".rodata": {"size": 12000}}},
        {"name": "IRAM", "used": 7028, "parts": {".text": {"size": 7028}}},
        {"name": "DRAM", "used": 9720, "parts": {".bss": {"size": 7700}, ".data": {"size": 2020}}},
        {"name": "RTC SLOW", "used": 64, "parts": {".force_slow": {"size": 40}}},
    ],
}

ARCHIVES = {
    "esp-idf/main/libmain.a": {
        "abbrev_name": "main",
        "size": 1000,
        "memory_types": {"Flash Code": {"size": 900}, "DRAM": {"size": 100}},
    },
    "esp-idf/honch/libhonch.a": {
        "abbrev_name": "honch",
        "size": 13939,
        "memory_types": {
            "Flash Code": {"size": 12191},
            "DRAM": {".dram0.bss": 1700, ".dram0.data": 20, "size": 1720},
            "IRAM": {"size": 28},
        },
    },
}


class FootprintMeasurementTest(unittest.TestCase):
    def write_json(self, directory: Path, name: str, data: dict) -> Path:
        path = directory / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_parse_total_sizes_extracts_marketing_relevant_memory_buckets(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_json(Path(tmp), "total.json", TOTAL_HONCH)

            sizes = parse_total_sizes(path)

        self.assertEqual(sizes["app_image_bytes"], 113500)
        self.assertEqual(sizes["flash_code_bytes"], 61000)
        self.assertEqual(sizes["flash_data_bytes"], 12000)
        self.assertEqual(sizes["iram_bytes"], 7028)
        self.assertEqual(sizes["dram_bytes"], 9720)
        self.assertEqual(sizes["static_ram_bytes"], 16812)

    def test_parse_archive_sizes_extracts_direct_honch_component_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_json(Path(tmp), "archives.json", ARCHIVES)

            honch = parse_archive_sizes(path, "esp-idf/honch/libhonch.a")

        self.assertEqual(honch["total_bytes"], 13939)
        self.assertEqual(honch["flash_code_bytes"], 12191)
        self.assertEqual(honch["dram_bytes"], 1720)
        self.assertEqual(honch["iram_bytes"], 28)

    def test_build_report_computes_deltas_and_threshold_copy(self):
        report = build_report(
            target="esp32",
            bare=parse_total_sizes.from_data(TOTAL_BARE),
            honch=parse_total_sizes.from_data(TOTAL_HONCH),
            honch_archive=parse_archive_sizes.from_data(ARCHIVES, "esp-idf/honch/libhonch.a"),
            runtime=None,
        )

        self.assertEqual(report["delta"]["app_image_bytes"], 13500)
        self.assertEqual(report["delta"]["static_ram_bytes"], 1748)
        self.assertEqual(report["direct_honch_archive"]["total_bytes"], 13939)
        self.assertEqual(report["landing_claims"]["sdk_flash"], "<20 KB")
        self.assertEqual(report["landing_claims"]["sdk_static_ram"], "<2 KB")

    def test_build_report_does_not_use_negative_connected_delta_for_claims(self):
        connected = parse_total_sizes.from_data(TOTAL_HONCH)
        honch = connected.copy()
        honch["app_image_bytes"] -= 5000
        honch["dram_bytes"] -= 1000
        honch["static_ram_bytes"] -= 1000

        report = build_report(
            target="esp32",
            bare=parse_total_sizes.from_data(TOTAL_BARE),
            connected=connected,
            honch=honch,
            honch_archive=parse_archive_sizes.from_data(ARCHIVES, "esp-idf/honch/libhonch.a"),
            runtime=None,
        )

        self.assertFalse(report["connected_delta_valid_for_claims"])
        self.assertEqual(report["landing_claims"]["sdk_flash"], "<20 KB")
        self.assertEqual(report["landing_claims"]["sdk_static_ram"], "<2 KB")

    def test_parse_monitor_log_extracts_heap_and_cpu_markers(self):
        text = """
I (100) honch_footprint: HONCH_FOOTPRINT_RUNTIME phase=before_honch heap_free=180000 heap_min=179000 heap_largest=120000 stack_high_water=2500
I (200) honch_footprint: HONCH_FOOTPRINT_RUNTIME phase=after_honch_init heap_free=172000 heap_min=169500 heap_largest=112000 stack_high_water=2100
I (300) honch_footprint: HONCH_FOOTPRINT_CPU samples=100 avg_us=76 min_us=40 p50_us=70 p95_us=110 max_us=200 cpu_pct_1hz=0.007600
"""
        runtime = parse_monitor_log(text)

        self.assertEqual(runtime["before_honch"]["heap_free"], 180000)
        self.assertEqual(runtime["after_honch_init"]["heap_free"], 172000)
        self.assertEqual(runtime["heap_delta_after_init_bytes"], 8000)
        self.assertEqual(runtime["track_cpu"]["avg_us"], 76)
        self.assertEqual(runtime["track_cpu"]["cpu_pct_1hz"], 0.0076)

    def test_tool_targets_refactored_esp_idf_footprint_app(self):
        self.assertEqual(measure.FOOTPRINT_DIR, REPO_ROOT / "ports" / "esp-idf" / "footprint")

    def test_footprint_registers_honch_requirement_before_variant_defines(self):
        cmake = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertIn("component requirements are collected during early expansion", cmake)
        self.assertIn("PRIV_REQUIRES honch esp_event esp_http_client esp_netif esp_timer esp_wifi heap nvs_flash", cmake)
        self.assertLess(cmake.index("idf_component_register("), cmake.index("if(HONCH_FOOTPRINT_WITH_SDK)"))

    def test_footprint_app_uses_honch_component_directory_for_archive_attribution(self):
        cmake = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertIn('set(EXTRA_COMPONENT_DIRS "../honch")', cmake)
        self.assertNotIn('set(EXTRA_COMPONENT_DIRS "../../..")', cmake)

    def test_runtime_cpu_sample_stays_on_ram_hot_path_before_api_setup(self):
        app = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "main" / "app_main.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("static uint8_t s_event_buffer[32768];", app)
        self.assertLess(app.index("run_honch_cpu_sample();"), app.index("run_representative_api_sample();"))

    def test_footprint_gpio_tracking_is_optional(self):
        root_cmake = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        main_cmake = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        app = (REPO_ROOT / "ports" / "esp-idf" / "footprint" / "main" / "app_main.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("HONCH_FOOTPRINT_INCLUDE_GPIO OFF", root_cmake)
        self.assertIn("FOOTPRINT_INCLUDE_GPIO=1", main_cmake)
        self.assertIn("#if FOOTPRINT_INCLUDE_GPIO", app)
        self.assertIn("honch_track_gpio(GPIO_NUM_0", app)

    def test_esp_ram_queue_metadata_uses_event_buffer_not_static_bss(self):
        header = (REPO_ROOT / "ports" / "esp-idf" / "honch" / "src" / "esp_core_adapter.h").read_text(
            encoding="utf-8"
        )
        storage = (REPO_ROOT / "ports" / "esp-idf" / "honch" / "src" / "esp_storage_nvs.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("honch_esp_ram_queue_entry_t *ram_entries;", header)
        self.assertIn("size_t ram_entry_capacity;", header)
        self.assertIn("#define HONCH_ESP_RAM_QUEUE_MAX_EVENTS 256u", header)
        self.assertNotIn("ram_entries[HONCH_ESP_RAM_QUEUE_MAX_EVENTS]", header)
        self.assertIn("ctx->ram_entries = (honch_esp_ram_queue_entry_t *)entries_start;", storage)


if __name__ == "__main__":
    unittest.main()
