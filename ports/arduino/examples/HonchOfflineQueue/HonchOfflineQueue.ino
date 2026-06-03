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

  HonchConfig config = {};
  config.apiKey = HONCH_PROJECT_KEY;
  config.host = "https://capture.honch.io";
  config.rootCaPem = HONCH_ROOT_CA_PEM;
  config.deviceModel = "esp32-devkit";
  config.firmwareVersion = "1.0.0";
  config.environment = "production";
  config.eventBuffer = eventBuffer;
  config.eventBufferSize = sizeof(eventBuffer);
  config.flushIntervalSeconds = 60;
  config.flushEventThreshold = 30;
  config.insecureSkipTlsVerify = false;

  honch::defaultClient().begin(config);
  honch::defaultClient().track("staged_before_wifi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  honch::defaultClient().flush();
}

void loop() {
  honch::defaultClient().tick();
}
