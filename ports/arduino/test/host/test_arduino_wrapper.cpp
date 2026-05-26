#include <assert.h>
#include <string.h>

#include "../../src/Honch.h"
#include "../../src/honch_arduino_adapter.h"

int main() {
  static uint8_t buffer[8192];
  HonchConfig config = {
    .apiKey = "test-key",
    .host = "http://127.0.0.1:8001",
    .rootCaPem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n",
    .deviceModel = "host-esp32",
    .firmwareVersion = "1.0.0",
    .environment = "test",
    .eventBuffer = buffer,
    .eventBufferSize = sizeof(buffer),
    .flushIntervalSeconds = 60,
    .flushEventThreshold = 30,
    .insecureSkipTlsVerify = true,
  };

  assert(Honch.begin(config));
  assert(Honch.track("button_pressed", "{\"count\":1}"));
  assert(Honch.identify("user-1", "{\"role\":\"tester\"}"));
  assert(Honch.setProperty("mode", "\"host\""));
  assert(Honch.sessionStart("demo"));
  assert(Honch.sessionEnd());
  assert(Honch.flush());
  assert(strlen(Honch.deviceId()) > 0);
  assert(strcmp(Honch.lastError(), "ok") == 0);
  assert(Honch.shutdown());

  honch_arduino_host_transport_reset();
  honch_arduino_host_transport_set_result(HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
  assert(Honch.begin(config));
  assert(Honch.track("queued", "{}"));
  assert(!Honch.shutdown());

  honch_arduino_host_transport_set_result(HONCH_OK, HONCH_TRANSPORT_ACCEPTED);
  assert(Honch.begin(config));
  assert(Honch.flush());
  assert(honch_arduino_host_transport_call_count() > 0);
  assert(strcmp(honch_arduino_host_transport_last_url(), "http://127.0.0.1:8001/capture") == 0);
  assert(strcmp(honch_arduino_host_transport_last_content_type(), "application/vnd.honch.chunk") == 0);
  assert(strcmp(honch_arduino_host_transport_last_project_key(), "test-key") == 0);
  assert(strlen(honch_arduino_host_transport_last_stream_id()) > 0);
  assert(honch_arduino_host_transport_last_body_size() > 0);
  assert(honch_arduino_host_transport_last_body() != NULL);
  assert(Honch.shutdown());
  return 0;
}
