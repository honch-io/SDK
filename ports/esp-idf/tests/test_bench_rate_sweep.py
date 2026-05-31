from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class EspIdfBenchRateSweepTests(unittest.TestCase):
    def test_rate_sweep_app_emits_window_schema(self) -> None:
        app = read("ports/esp-idf/rate_sweep_bench/main/app_main.c")

        self.assertIn("BENCH_SUITE_START", app)
        self.assertIn("BENCH_RATE_START", app)
        self.assertIn("BENCH_WINDOW", app)
        self.assertIn("BENCH_RATE_END", app)
        self.assertIn("BENCH_SUITE_END", app)
        self.assertIn("honch-rate-sweep-v1", app)
        self.assertIn("rate_target_eps", app)
        self.assertIn("actual_eps", app)
        self.assertIn("track_p95_us", app)
        self.assertIn("track_p99_us", app)
        self.assertIn("cpu_percent", app)

    def test_rate_sweep_defaults_enable_cpu_runtime_stats(self) -> None:
        defaults = read("ports/esp-idf/rate_sweep_bench/sdkconfig.defaults")

        self.assertIn("CONFIG_FREERTOS_USE_TRACE_FACILITY=y", defaults)
        self.assertIn("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y", defaults)
        self.assertIn("CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y", defaults)

    def test_rate_sweep_defaults_are_configurable(self) -> None:
        kconfig = read("ports/esp-idf/rate_sweep_bench/main/Kconfig.projbuild")

        self.assertIn("RATE_SWEEP_RATES", kconfig)
        self.assertIn("RATE_SWEEP_DURATION_SECONDS", kconfig)
        self.assertIn("RATE_SWEEP_WARMUP_SECONDS", kconfig)
        self.assertIn("RATE_SWEEP_WINDOW_SECONDS", kconfig)
        self.assertIn("RATE_SWEEP_FLUSH_TIMING", kconfig)

    def test_rate_sweep_flush_counter_spans_windows(self) -> None:
        app = read("ports/esp-idf/rate_sweep_bench/main/app_main.c")

        self.assertIn("static uint32_t s_accepted_since_flush", app)
        self.assertNotIn("uint32_t accepted_since_flush = 0;", app)
        self.assertIn("s_accepted_since_flush >= CONFIG_RATE_SWEEP_FLUSH_EVERY", app)

    def test_rate_sweep_cpu_uses_all_core_wall_capacity(self) -> None:
        app = read("ports/esp-idf/rate_sweep_bench/main/app_main.c")

        self.assertIn("wall_time_us", app)
        self.assertIn("configNUMBER_OF_CORES", app)
        self.assertIn("capacity_delta", app)
        self.assertIn("idle_delta > capacity_delta", app)
        self.assertNotIn("busy_delta = total_delta > idle_delta", app)

    def test_legacy_benchtest_is_not_the_rate_sweep_app(self) -> None:
        benchtest = read("ports/esp-idf/benchtest/main/app_main.c")

        self.assertNotIn("BENCH_SUITE_START", benchtest)
        self.assertNotIn("BENCH_WINDOW", benchtest)


if __name__ == "__main__":
    unittest.main()
