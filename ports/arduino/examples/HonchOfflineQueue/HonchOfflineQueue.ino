#include <WiFi.h>
#include <Honch.h>

static const char *WIFI_SSID = "ssid";
static const char *WIFI_PASSWORD = "password";
static const char *HONCH_PROJECT_KEY = "project-key";

static uint8_t eventBuffer[8192];

void setup() {
  Serial.begin(115200);

  HonchConfig config = {};
  config.apiKey = HONCH_PROJECT_KEY;
  config.host = "https://capture.honch.io";
  config.deviceModel = "esp32-devkit";
  config.firmwareVersion = "1.0.0";
  config.environment = "production";
  config.eventBuffer = eventBuffer;
  config.eventBufferSize = sizeof(eventBuffer);
  config.flushIntervalSeconds = 60;
  config.flushEventThreshold = 30;
  config.insecureSkipTlsVerify = false;

  Honch.begin(config);
  Honch.track("staged_before_wifi", "{}");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  Honch.flush();
}

void loop() {}
