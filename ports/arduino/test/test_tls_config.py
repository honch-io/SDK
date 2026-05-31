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

    def test_public_config_exposes_flush_spacing(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("uint32_t flushMinIntervalMs;", header)
        self.assertIn("coreConfig.flush_min_interval_ms = config.flushMinIntervalMs;", adapter)
        self.assertIn("flushMinIntervalMs", readme)
        self.assertIn("HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS", readme)

    def test_public_config_exposes_connectivity_gate(self) -> None:
        header = read("ports/arduino/src/Honch.h")
        adapter = read("ports/arduino/src/Honch.cpp")
        readme = read("ports/arduino/README.md")

        self.assertIn("bool (*connectivityCallback)();", header)
        self.assertIn("honch_arduino_connectivity_callback", adapter)
        self.assertIn("coreConfig.connectivity_callback = honch_arduino_connectivity_callback;", adapter)
        self.assertIn("connectivityCallback", readme)
        self.assertIn("offline", readme)

    def test_transport_initializer_matches_core_ops_shape(self) -> None:
        transport = read("ports/arduino/src/honch_arduino_transport.cpp")

        self.assertIn("arduino_post_chunk,\n      nullptr,\n      ctx,", transport)


if __name__ == "__main__":
    unittest.main()
