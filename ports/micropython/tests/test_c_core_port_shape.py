import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MICROPYTHON = ROOT / "ports" / "micropython"
USERMOD = MICROPYTHON / "usermod" / "honch"


class MicroPythonCCorePortShapeTests(unittest.TestCase):
    def read(self, relative):
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_user_c_module_declares_core_sources(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")

        self.assertIn("INTERFACE_LIBRARY", cmake)
        self.assertIn("honch_micropython", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_core.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_cbor.c", cmake)
        self.assertIn("${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_packetizer.c", cmake)

    def test_rp2_build_reserves_c_heap_for_core_allocations(self):
        cmake = self.read("ports/micropython/usermod/honch/micropython.cmake")
        makefile = self.read("ports/micropython/usermod/honch/micropython.mk")

        self.assertIn("set(MICROPY_C_HEAP_SIZE 65536)", cmake)
        self.assertIn("MICROPY_C_HEAP_SIZE=65536", cmake)
        self.assertIn("-DMICROPY_C_HEAP_SIZE=65536", makefile)

    def test_c_binding_registers_honch_core_module_and_delegates_to_c_core(self):
        module = self.read("ports/micropython/usermod/honch/modhonch_core.c")

        self.assertIn('MP_REGISTER_MODULE(MP_QSTR__honch_core', module)
        self.assertIn("honch_core_init(&self->client", module)
        self.assertIn("honch_core_track(self->client", module)
        self.assertIn("honch_core_identify(self->client", module)
        self.assertIn("honch_core_flush(self->client", module)
        self.assertIn("honch_core_shutdown(self->client", module)

    def test_micropython_adapters_implement_core_ops(self):
        adapters = "\n".join(
            [
                self.read("ports/micropython/usermod/honch/mphal_adapter.c"),
                self.read("ports/micropython/usermod/honch/mpstorage_adapter.c"),
                self.read("ports/micropython/usermod/honch/mptransport_adapter.c"),
            ]
        )

        self.assertIn("honch_micropython_platform_ops_init", adapters)
        self.assertIn("honch_micropython_storage_ops_init", adapters)
        self.assertIn("honch_micropython_transport_ops_init", adapters)
        self.assertIn("honch_platform_ops_t", adapters)
        self.assertIn("honch_storage_ops_t", adapters)
        self.assertIn("honch_transport_ops_t", adapters)

    def test_python_client_is_thin_wrapper_over_c_module(self):
        client = self.read("ports/micropython/honch/client.py")

        self.assertIn("import _honch_core", client)
        self.assertIn("_honch_core.Client", client)
        self.assertNotIn("PersistentQueue", client)
        self.assertNotIn("IdentityStore", client)
        self.assertNotIn("build_event", client)
        self.assertNotIn("post_batch", client)


if __name__ == "__main__":
    unittest.main()
