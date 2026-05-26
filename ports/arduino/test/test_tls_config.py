from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class ArduinoTLSConfigTests(unittest.TestCase):
    def test_public_config_exposes_root_ca_pem(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/honch_arduino_adapter.h")

        self.assertIn("const char *rootCaPem;", header)
        self.assertIn("const char *rootCaPem;", adapter)

    def test_secure_transport_uses_root_ca_before_insecure_fallback(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")

        self.assertIn("secureClient.setCACert(transport->rootCaPem);", transport)
        self.assertLess(
            transport.index("secureClient.setCACert(transport->rootCaPem);"),
            transport.index("secureClient.setInsecure();"),
        )

    def test_examples_configure_tls_root_ca_not_insecure_skip(self) -> None:
        basic = read("ports/arduino/examples/HonchBasic/HonchBasic.ino")
        offline = read("ports/arduino/examples/HonchOfflineQueue/HonchOfflineQueue.ino")

        for sketch in (basic, offline):
            self.assertIn("static const char HONCH_ROOT_CA_PEM[]", sketch)
            self.assertIn("config.rootCaPem = HONCH_ROOT_CA_PEM;", sketch)
            self.assertIn("config.insecureSkipTlsVerify = false;", sketch)


if __name__ == "__main__":
    unittest.main()
