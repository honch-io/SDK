from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def canonical_version() -> str:
    """The single source of truth: HONCH_SDK_VERSION in the canonical header."""
    match = re.search(r'#define HONCH_SDK_VERSION "([^"]+)"', read("core/src/honch_internal.h"))
    assert match, "HONCH_SDK_VERSION not found in core/src/honch_internal.h"
    return match.group(1)


class PosixInstallPackageTests(unittest.TestCase):
    def test_cmake_installs_library_headers_and_export(self) -> None:
        cmake = read("ports/posix/CMakeLists.txt")

        self.assertIn("include(GNUInstallDirs)", cmake)
        self.assertIn("install(TARGETS honch_posix", cmake)
        self.assertIn("EXPORT honch_posixTargets", cmake)
        self.assertIn("install(DIRECTORY include/", cmake)
        self.assertIn("install(EXPORT honch_posixTargets", cmake)

    def test_cmake_generates_package_config(self) -> None:
        cmake = read("ports/posix/CMakeLists.txt")
        config = read("ports/posix/cmake/honch_posixConfig.cmake.in")

        self.assertIn("configure_package_config_file", cmake)
        self.assertIn("write_basic_package_version_file", cmake)
        self.assertIn("honch_posixConfig.cmake", cmake)
        self.assertIn("include(CMakeFindDependencyMacro)", config)
        self.assertIn("find_dependency(CURL)", config)
        self.assertIn("find_dependency(Threads)", config)
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/honch_posixTargets.cmake")', config)

    def test_package_version_matches_documented_posix_version(self) -> None:
        cmake = read("ports/posix/CMakeLists.txt")
        root_readme = read("README.md")

        version = canonical_version()
        self.assertIn(f"project(honch_posix_sdk VERSION {version} LANGUAGES C)", cmake)
        self.assertIn(f"| C/POSIX | Stable | `{version}` | [`ports/posix/`](ports/posix/) |", root_readme)


if __name__ == "__main__":
    unittest.main()
