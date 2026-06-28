// Honch automatic crash reporting demo (Arduino-ESP32).
//
// Proves automatic $crash capture by deliberately crashing the firmware. With
// enableErrorTracking set and a coredump partition configured (see below), the
// ESP32 reset reason from the previous boot is mapped into a one-time $crash
// event emitted during begin() on the NEXT boot — no manual error API.
//
// Required build setup (PlatformIO platformio.ini or Arduino board options):
//   * a partition scheme that includes a `coredump` partition, and
//   * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y with the ELF coredump format,
//   so the panic handler writes a coredump the SDK can summarize next boot.
//
// First boot: prints, then crashes (null dereference) ~5s after connecting.
// Next boot: begin() reports the recovered $crash; watch it arrive at Capture.

#include <WiFi.h>
#include <Honch.h>

static const char *WIFI_SSID = "ssid";
static const char *WIFI_PASSWORD = "password";
static const char *HONCH_PROJECT_KEY = "project-key";
static const char HONCH_ROOT_CA_PEM[] = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_CAPTURE_ENDPOINT_ROOT_CA
-----END CERTIFICATE-----
)EOF";

static uint8_t eventBuffer[8192];

void setup() {
  Serial.begin(115200);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  HonchConfig config = {};
  config.apiKey = HONCH_PROJECT_KEY;
  config.host = "https://i.honch.io";
  config.rootCaPem = HONCH_ROOT_CA_PEM;
  config.deviceModel = "esp32-devkit";
  config.firmwareVersion = "1.0.0";
  config.environment = "dev";
  config.eventBuffer = eventBuffer;
  config.eventBufferSize = sizeof(eventBuffer);
  config.flushIntervalSeconds = 10;
  config.flushEventThreshold = 1;
  config.insecureSkipTlsVerify = false;
  config.enableErrorTracking = true;  // emit the recovered $crash after a crash

  honch::defaultClient().begin(config);
  honch::defaultClient().track("boot");

  // Deliver any recovered $crash from the previous boot before we crash again.
  honch::defaultClient().flush();

  Serial.println("HonchCrashDemo: crashing on purpose in 5s...");
  delay(5000);

  // Deliberate null dereference -> ESP32 panic -> coredump written to flash.
  // The NEXT boot's begin() reports this as a $crash.
  volatile int *boom = nullptr;
  *boom = 42;
}

void loop() {
  honch::defaultClient().tick();
}
