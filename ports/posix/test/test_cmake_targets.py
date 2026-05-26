from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


class PosixCMakeTargetTests(unittest.TestCase):
    def test_production_target_does_not_compile_with_testing_hooks(self) -> None:
        cmake = (ROOT / "ports/posix/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertNotIn(
            "target_compile_definitions(honch_posix PRIVATE HONCH_TESTING)",
            cmake,
        )
        self.assertIn("add_library(honch_posix_test", cmake)
        self.assertIn("target_compile_definitions(honch_posix_test PRIVATE", cmake)
        self.assertIn("HONCH_TESTING", cmake)
        self.assertIn("target_link_libraries(honch_posix_tests PRIVATE honch_posix_test)", cmake)

    def test_benchmark_target_uses_separate_instrumented_library(self) -> None:
        cmake = (ROOT / "ports/posix/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("add_library(honch_posix_bench_instrumented", cmake)
        self.assertIn("target_link_libraries(honch_posix_bench_instrumented", cmake)
        self.assertNotIn("target_link_libraries(honch_posix PRIVATE honch_bench_alloc_support)", cmake)


if __name__ == "__main__":
    unittest.main()

