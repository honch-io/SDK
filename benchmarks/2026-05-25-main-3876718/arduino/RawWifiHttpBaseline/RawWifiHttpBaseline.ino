#include <HTTPClient.h>
#include <WiFi.h>

#ifndef WIFI_SSID
#define WIFI_SSID "ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "password"
#endif

#ifndef HONCH_HOST
#define HONCH_HOST "http://192.168.1.122:8001"
#endif

static const int EVENT_COUNT = 20;
static const int PAYLOAD_BYTES = 64;

static void marker(const char *name) {
  Serial.print("ARDUINO_RAW ");
  Serial.println(name);
}

static void markerValue(const char *name, const char *key, long value) {
  Serial.print("ARDUINO_RAW ");
  Serial.print(name);
  Serial.print(" ");
  Serial.print(key);
  Serial.print("=");
  Serial.println(value);
}

static void printHeap(const char *label) {
  Serial.print("ARDUINO_RAW HEAP label=");
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
  Serial.print("ARDUINO_RAW WIFI connected=");
  Serial.print(WiFi.status() == WL_CONNECTED ? 1 : 0);
  Serial.print(" ip=");
  Serial.println(WiFi.localIP());
  return WiFi.status() == WL_CONNECTED;
}

static int getHealth() {
  HTTPClient http;
  String url = String(HONCH_HOST) + "/health";
  if (!http.begin(url)) {
    return -1;
  }
  int code = http.GET();
  http.end();
  return code;
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

  int health = getHealth();
  markerValue("HEALTH", "status", health);
  printHeap("after_http");

  char payload[PAYLOAD_BYTES + 1];
  memset(payload, 'x', PAYLOAD_BYTES);
  payload[PAYLOAD_BYTES] = '\0';

  unsigned long totalUs = 0;
  unsigned long minUs = 0xffffffffUL;
  unsigned long maxUs = 0;
  for (int i = 0; i < EVENT_COUNT; ++i) {
    unsigned long start = micros();
    String event = String("{\"seq\":") + i + ",\"payload\":\"" + payload + "\"}";
    size_t len = event.length();
    (void)len;
    unsigned long took = micros() - start;
    totalUs += took;
    if (took < minUs) {
      minUs = took;
    }
    if (took > maxUs) {
      maxUs = took;
    }
  }

  Serial.print("ARDUINO_RAW BUILD_SUMMARY count=");
  Serial.print(EVENT_COUNT);
  Serial.print(" avg_us=");
  Serial.print(totalUs / EVENT_COUNT);
  Serial.print(" min_us=");
  Serial.print(minUs);
  Serial.print(" max_us=");
  Serial.println(maxUs);
  printHeap("after_payload_build");
  marker("DONE");
}

void loop() {}
