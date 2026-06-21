# Honch automatic error & crash reporting demo (MicroPython).
#
# Demonstrates the two automatic capture paths with no manual error API:
#
#   * report_log_error(...)  -> a bounded, coalesced $error event. Wire a
#     logging.Handler to it to capture errors your firmware already logs.
#   * install_error_hook()   -> installs a sys.excepthook so an UNCAUGHT
#     exception is reported as a $crash (source="exception") and then re-raised
#     to the previous hook. No try/except needed at the call site.
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
)

# Any uncaught exception from here on is reported as a $crash.
client.install_error_hook()

# 1) Automatic $error: an error condition the firmware handled and logged.
client.report_log_error("temperature read failed: bus timeout", component="sensor")
client.flush()  # deliver the $error

# 2) Automatic $crash: raise an uncaught exception. The excepthook reports a
#    $crash (type="ValueError") and re-raises. On the next boot you would also
#    flush() to deliver it; here it is delivered before re-raise on platforms
#    whose excepthook runs before exit.
client.flush()
raise ValueError("intentional crash to prove $crash capture")
