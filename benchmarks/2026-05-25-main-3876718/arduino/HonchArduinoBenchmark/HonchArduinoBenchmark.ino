#include <WiFi.h>
#include <Honch.h>

#ifndef WIFI_SSID
#define WIFI_SSID "ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "password"
#endif

#ifndef HONCH_HOST
#define HONCH_HOST "http://192.168.1.122:8001"
#endif

#ifndef HONCH_PROJECT_KEY
#define HONCH_PROJECT_KEY "honch_e2e_test_key"
#endif

static const int EVENT_COUNT = 20;
static const int PAYLOAD_BYTES = 64;
static uint8_t eventBuffer[8192];

static void marker(const char *name) {
  Serial.print("ARDUINO_HONCH ");
  Serial.println(name);
}

static void printHeap(const char *label) {
  Serial.print("ARDUINO_HONCH HEAP label=");
  Serial.print(label);
  Serial.print(" free=");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" min=");
  Serial.print(ESP.getMinFreeHeap());
  Serial.print(" max_alloc=");
  Serial.println(ESP.getMaxAllocHeap());
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
  }
  Serial.print("ARDUINO_HONCH WIFI connected=");
  Serial.print(WiFi.status() == WL_CONNECTED ? 1 : 0);
  Serial.print(" ip=");
  Serial.println(WiFi.localIP());
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  marker("START");
  printHeap("boot");

  bool wifiOk = connectWifi();
  printHeap("after_wifi");
  if (!wifiOk) {
    marker("DONE");
    return;
  }

  HonchConfig config = {};
  config.apiKey = HONCH_PROJECT_KEY;
  config.host = HONCH_HOST;
  config.deviceModel = "arduino-esp32-benchmark";
  config.firmwareVersion = "arduino-bench-main-3876718";
  config.environment = "benchmark";
  config.eventBuffer = eventBuffer;
  config.eventBufferSize = sizeof(eventBuffer);
  config.flushIntervalSeconds = 60;
  config.flushEventThreshold = 10;
  config.insecureSkipTlsVerify = true;

  unsigned long initStart = micros();
  bool initOk = Honch.begin(config);
  unsigned long initUs = micros() - initStart;
  Serial.print("ARDUINO_HONCH INIT ok=");
  Serial.print(initOk ? 1 : 0);
  Serial.print(" us=");
  Serial.print(initUs);
  Serial.print(" error=");
  Serial.println(Honch.lastError());
  printHeap("after_init");

  char payload[PAYLOAD_BYTES + 1];
  memset(payload, 'x', PAYLOAD_BYTES);
  payload[PAYLOAD_BYTES] = '\0';

  unsigned long totalUs = 0;
  unsigned long minUs = 0xffffffffUL;
  unsigned long maxUs = 0;
  int failures = 0;
  for (int i = 0; i < EVENT_COUNT; ++i) {
    char properties[128];
    snprintf(properties, sizeof(properties), "{\"seq\":%d,\"payload\":\"%s\"}", i, payload);
    unsigned long start = micros();
    bool ok = Honch.track("arduino_esp32_benchmark", properties);
    unsigned long took = micros() - start;
    if (!ok) {
      failures++;
    }
    totalUs += took;
    if (took < minUs) {
      minUs = took;
    }
    if (took > maxUs) {
      maxUs = took;
    }
  }

  Serial.print("ARDUINO_HONCH TRACK_SUMMARY count=");
  Serial.print(EVENT_COUNT);
  Serial.print(" failures=");
  Serial.print(failures);
  Serial.print(" avg_us=");
  Serial.print(totalUs / EVENT_COUNT);
  Serial.print(" min_us=");
  Serial.print(minUs);
  Serial.print(" max_us=");
  Serial.println(maxUs);
  printHeap("after_track");

  unsigned long flushStart = micros();
  bool flushOk = Honch.flush();
  unsigned long flushUs = micros() - flushStart;
  Serial.print("ARDUINO_HONCH FLUSH ok=");
  Serial.print(flushOk ? 1 : 0);
  Serial.print(" us=");
  Serial.print(flushUs);
  Serial.print(" error=");
  Serial.println(Honch.lastError());
  printHeap("after_flush");

  unsigned long shutdownStart = micros();
  bool shutdownOk = Honch.shutdown();
  unsigned long shutdownUs = micros() - shutdownStart;
  Serial.print("ARDUINO_HONCH SHUTDOWN ok=");
  Serial.print(shutdownOk ? 1 : 0);
  Serial.print(" us=");
  Serial.print(shutdownUs);
  Serial.print(" error=");
  Serial.println(Honch.lastError());
  printHeap("after_shutdown");
  marker("DONE");
}

void loop() {}
