#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../src/Honch.h"
#include "../../src/honch_arduino_adapter.h"

static void assert_http_status(int code, honch_status_t status, honch_transport_result_t result) {
  honch_transport_result_t actualResult = HONCH_TRANSPORT_RETRY;
  assert(honch_arduino_host_classify_http_status(code, &actualResult) == status);
  assert(actualResult == result);
}

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

  assert(!Honch.tick());
  assert(strcmp(Honch.lastError(), "not initialized") == 0);

  assert(Honch.begin(config));
  const honch_property_t buttonProps[] = {
      honch_prop("count", honch_i64(1))
  };
  const honch_property_t traits[] = {
      honch_prop("role", honch_str("tester"))
  };
  assert(Honch.track("button_pressed", buttonProps, 1u));
  assert(Honch.identify("user-1", traits, 1u));
  assert(Honch.setProperty("mode", honch_str("host")));
  assert(Honch.sessionStart("demo"));
  assert(Honch.sessionEnd());
  if (!Honch.flush()) {
    fprintf(stderr, "flush failed: %s\n", Honch.lastError());
    abort();
  }
  assert(strlen(Honch.deviceId()) > 0);
  assert(strcmp(Honch.lastError(), "ok") == 0);
  assert(Honch.shutdown());

  honch_arduino_host_transport_reset();
  honch_arduino_host_transport_set_result(HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
  assert(Honch.begin(config));
  assert(Honch.track("queued"));
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

  assert_http_status(202, HONCH_OK, HONCH_TRANSPORT_CHUNK_STORED);
  assert_http_status(204, HONCH_OK, HONCH_TRANSPORT_ACCEPTED);
  assert_http_status(400, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
  assert_http_status(401, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_AUTH_ERROR);
  assert_http_status(408, HONCH_ERROR_TIMEOUT, HONCH_TRANSPORT_RETRY);
  assert_http_status(409, HONCH_ERROR_TRANSPORT, HONCH_TRANSPORT_RETRY);
  assert_http_status(413, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
  assert_http_status(415, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
  assert_http_status(422, HONCH_ERROR_REJECTED, HONCH_TRANSPORT_REJECTED);
  assert_http_status(429, HONCH_ERROR_RATE_LIMITED, HONCH_TRANSPORT_RETRY);
  assert_http_status(500, HONCH_ERROR_SERVER, HONCH_TRANSPORT_RETRY);

  HonchConfig scheduledConfig = config;
  scheduledConfig.flushIntervalSeconds = 2;
  scheduledConfig.flushEventThreshold = 3;
  honch_arduino_host_transport_reset();
  honch_arduino_host_set_millis(1000);
  assert(Honch.begin(scheduledConfig));
  assert(Honch.track("scheduled_one"));
  assert(Honch.tick());
  assert(honch_arduino_host_transport_call_count() == 0);
  assert(Honch.track("scheduled_two"));
  assert(Honch.tick());
  assert(honch_arduino_host_transport_call_count() > 0);
  assert(Honch.shutdown());

  honch_arduino_host_transport_reset();
  honch_arduino_host_set_millis(5000);
  assert(Honch.begin(scheduledConfig));
  assert(Honch.track("interval_one"));
  honch_arduino_host_advance_millis(1000);
  assert(Honch.loop());
  assert(honch_arduino_host_transport_call_count() == 0);
  honch_arduino_host_advance_millis(1000);
  assert(Honch.loop());
  assert(honch_arduino_host_transport_call_count() > 0);
  assert(Honch.shutdown());

  HonchConfig defaultScheduledConfig = config;
  defaultScheduledConfig.flushIntervalSeconds = 0;
  defaultScheduledConfig.flushEventThreshold = 0;
  honch_arduino_host_transport_reset();
  honch_arduino_host_set_millis(10000);
  assert(Honch.begin(defaultScheduledConfig));
  assert(Honch.track("default_interval"));
  honch_arduino_host_advance_millis(59000);
  assert(Honch.tick());
  assert(honch_arduino_host_transport_call_count() == 0);
  honch_arduino_host_advance_millis(1000);
  assert(Honch.tick());
  assert(honch_arduino_host_transport_call_count() > 0);
  assert(Honch.shutdown());

  honch_platform_ops_t platformOps;
  honch_arduino_platform_t platformCtx;
  assert(honch_arduino_platform_ops_init(&platformOps, &platformCtx) == HONCH_OK);
  honch_arduino_host_set_millis(12000);
  assert(platformOps.now_ms(platformOps.ctx) == 12000);
  assert(platformOps.uptime_ms(platformOps.ctx) == 12000);
  honch_arduino_host_set_epoch_millis(1700000012000ULL);
  assert(platformOps.now_ms(platformOps.ctx) == 1700000012000ULL);
  assert(platformOps.uptime_ms(platformOps.ctx) == 12000);
  honch_arduino_host_clear_epoch_millis();
  return 0;
}
