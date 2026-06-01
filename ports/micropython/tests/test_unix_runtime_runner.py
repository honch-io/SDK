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

    def test_github_workflow_builds_unix_port_with_honch_core(self) -> None:
        workflow = read(".github/workflows/micropython.yml")

        self.assertIn("Build MicroPython unix port with _honch_core", workflow)
        self.assertIn("micropython/micropython", workflow)
        self.assertIn("make -C micropython/mpy-cross", workflow)
        self.assertIn("USER_C_MODULES=$GITHUB_WORKSPACE/ports/micropython/usermod", workflow)
        self.assertIn("ports/micropython/scripts/run-unix-tests.sh", workflow)

    def test_runtime_smoke_uses_real_honch_wrapper(self) -> None:
        smoke = read("ports/micropython/tests/runtime_smoke.py")

        self.assertIn("import honch", smoke)
        self.assertIn("honch.Honch(", smoke)
        self.assertIn("client.track(", smoke)
        self.assertIn("except honch.TransportError", smoke)
        self.assertIn("client.shutdown()", smoke)


if __name__ == "__main__":
    unittest.main()
