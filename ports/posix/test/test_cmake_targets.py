from pathlib import Path
import subprocess
import tempfile
import textwrap
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

        self.assertIn("honch_core_bench_objects", cmake)
        self.assertIn("add_library(honch_posix_bench_instrumented", cmake)
        self.assertIn("target_link_libraries(honch_posix_bench_instrumented", cmake)
        self.assertNotIn("target_link_libraries(honch_posix PRIVATE honch_bench_alloc_support)", cmake)

    def test_benchmark_allocator_does_not_instrument_shared_core_target(self) -> None:
        cmake = (ROOT / "core/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("honch_configure_core_objects(honch_core_objects)", cmake)
        self.assertIn("honch_configure_core_objects(honch_core_bench_objects)", cmake)
        self.assertIn("target_compile_options(honch_core_bench_objects PRIVATE", cmake)
        self.assertNotIn("target_compile_options(honch_core_objects PRIVATE\n        -include", cmake)

    def test_posix_benchmarks_configure_when_parent_preloads_core_without_benchmarks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source_dir = Path(temp_dir) / "source"
            build_dir = Path(temp_dir) / "build"
            source_dir.mkdir()
            (source_dir / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    f"""
                    cmake_minimum_required(VERSION 3.20)
                    project(honch_posix_parent_preload C)

                    set(HONCH_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
                    add_subdirectory({(ROOT / "core").as_posix()} core-build)

                    set(HONCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
                    set(HONCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
                    set(HONCH_BUILD_BENCHMARKS ON CACHE BOOL "" FORCE)
                    add_subdirectory({(ROOT / "ports/posix").as_posix()} posix-build)
                    """
                ).lstrip(),
                encoding="utf-8",
            )

            command = ["cmake", "-S", str(source_dir), "-B", str(build_dir)]
            sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
            if sdk.exists():
                command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")

            result = subprocess.run(command, text=True, capture_output=True, check=False)

        self.assertEqual(
            0,
            result.returncode,
            result.stdout + result.stderr,
        )


if __name__ == "__main__":
    unittest.main()
