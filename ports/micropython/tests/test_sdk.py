import unittest


class LegacyPurePythonSdkTests(unittest.TestCase):
    @unittest.skip("MicroPython SDK behavior is now owned by the _honch_core user C module")
    def test_legacy_pure_python_suite_removed(self):
        pass


if __name__ == "__main__":
    unittest.main()
