# Honch crash & error reporting demo (MicroPython).
#
# Shows the two capture paths:
#   * report_log_error(...) -> a bounded, coalesced $error for an error your
#     firmware handled (wire it into your logging).
#   * client.run(main)      -> runs your entry point; an UNCAUGHT exception is
#     reported as a $crash (with the Python traceback), flushed, and re-raised.
#     This works on EVERY MicroPython build. If your firmware was built with
#     MICROPY_PY_SYS_EXCEPTHOOK you can call client.install_error_hook() instead
#     of wrapping main -- but stock firmware (e.g. the Pico W) lacks it, so run()
#     is the portable choice.
#
# Run on a Pico W / ESP32 MicroPython build that includes the _honch_core user
# C module. Fill in secrets.py first (see secrets.example.py).

import honch

try:
    from secrets import HONCH_API_KEY, HONCH_ENDPOINT_URL
except ImportError:
    HONCH_API_KEY = "local-dev-key"
    HONCH_ENDPOINT_URL = "http://127.0.0.1:8765"

client = honch.Honch(
    api_key=HONCH_API_KEY,
    endpoint_url=HONCH_ENDPOINT_URL,
    device_id="crash-demo-device",
    device_model="Crash Demo Rig",
    firmware_version="1.0.0",
    environment="dev",
    event_buffer=bytearray(8192),
)


def main():
    # 1) Handled, non-fatal error -> a $error event (queued).
    client.report_log_error("temperature read failed: bus timeout", component="sensor")

    # 2) Uncaught exception -> a $crash carrying the type, message, and Python
    #    traceback. client.run() reports it, flushes everything queued (the $error
    #    above and this $crash), and re-raises.
    raise ValueError("intentional crash to prove $crash capture")


# Wrap the entry point so the crash above is captured on ANY firmware build.
client.run(main)
