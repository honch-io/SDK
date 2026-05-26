from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class MicroPythonUnixRuntimeRunnerTests(unittest.TestCase):
    def test_runner_requires_honch_core_and_runs_runtime_smoke(self) -> None:
        script = read("ports/micropython/scripts/run-unix-tests.sh")

        self.assertIn('import _honch_core', script)
        self.assertIn("MICROPYPATH=", script)
        self.assertIn("tests/runtime_smoke.py", script)

    def test_runtime_smoke_uses_real_honch_wrapper(self) -> None:
        smoke = read("ports/micropython/tests/runtime_smoke.py")

        self.assertIn("import honch", smoke)
        self.assertIn("honch.Honch(", smoke)
        self.assertIn("client.track(", smoke)
        self.assertIn("client.shutdown()", smoke)


if __name__ == "__main__":
    unittest.main()
