#include "honch_arduino_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#ifdef ARDUINO
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#endif

namespace {

#ifndef ARDUINO
struct HostTransportState {
  honch_status_t status = HONCH_OK;
  honch_transport_result_t result = HONCH_TRANSPORT_ACCEPTED;
  size_t calls = 0;
  std::string url;
  std::string contentType;
  std::string projectKey;
  std::string streamId;
  std::vector<uint8_t> body;
};

HostTransportState g_hostTransport;
#endif

std::string capture_url(const char *host) {
  std::string url = host == nullptr ? "" : host;
  while (!url.empty() && url[url.size() - 1] == '/') {
    url.erase(url.size() - 1);
  }
  url += "/capture";
  return url;
}

honch_status_t arduino_post_chunk(
    void *ctx,
    const char *endpointUrl,
    const char *apiKey,
    const char *streamId,
    const uint8_t *body,
    size_t bodySize,
    honch_transport_result_t *result) {
  honch_arduino_transport_t *transport = static_cast<honch_arduino_transport_t *>(ctx);
  (void)transport;
  if (endpointUrl == nullptr || apiKey == nullptr || body == nullptr || bodySize == 0 || result == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  std::string url = capture_url(endpointUrl);
  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool https = url.rfind("https://", 0) == 0;
  if (https) {
    if (transport != nullptr && transport->insecureSkipTlsVerify) {
      secureClient.setInsecure();
    }
    if (!http.begin(secureClient, url.c_str())) {
      *result = HONCH_TRANSPORT_RETRY;
      return HONCH_ERROR_TRANSPORT;
    }
  } else {
    if (!http.begin(plainClient, url.c_str())) {
      *result = HONCH_TRANSPORT_RETRY;
      return HONCH_ERROR_TRANSPORT;
    }
  }

  http.addHeader("Content-Type", "application/vnd.honch.chunk");
  http.addHeader("X-Honch-Project-Key", apiKey);
  if (streamId != nullptr && streamId[0] != '\0') {
    http.addHeader("X-Honch-Stream-Id", streamId);
  }

  int code = http.POST(const_cast<uint8_t *>(body), bodySize);
  http.end();
  if (code >= 200 && code < 300) {
    *result = HONCH_TRANSPORT_ACCEPTED;
    return HONCH_OK;
  }
  if (code == 400 || code == 401 || code == 404) {
    *result = code == 401 ? HONCH_TRANSPORT_AUTH_ERROR : HONCH_TRANSPORT_REJECTED;
    return HONCH_ERROR_REJECTED;
  }
  *result = HONCH_TRANSPORT_RETRY;
  if (code == 429) {
    return HONCH_ERROR_RATE_LIMITED;
  }
  if (code >= 500) {
    return HONCH_ERROR_SERVER;
  }
  return HONCH_ERROR_TRANSPORT;
#else
  g_hostTransport.calls++;
  g_hostTransport.url = capture_url(endpointUrl);
  g_hostTransport.contentType = "application/vnd.honch.chunk";
  g_hostTransport.projectKey = apiKey;
  g_hostTransport.streamId = streamId == nullptr ? "" : streamId;
  g_hostTransport.body.assign(body, body + bodySize);
  *result = g_hostTransport.result;
  return g_hostTransport.status;
#endif
}

} // namespace

honch_status_t honch_arduino_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_arduino_transport_t *ctx,
    const HonchConfig &config) {
  if (ops == nullptr || ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ctx->apiKey = config.apiKey;
  ctx->host = config.host;
  ctx->insecureSkipTlsVerify = config.insecureSkipTlsVerify;
  *ops = honch_transport_ops_t{
      arduino_post_chunk,
      ctx,
  };
  return HONCH_OK;
}

#ifndef ARDUINO
void honch_arduino_host_transport_reset(void) {
  g_hostTransport = HostTransportState{};
}

size_t honch_arduino_host_transport_call_count(void) {
  return g_hostTransport.calls;
}

const char *honch_arduino_host_transport_last_url(void) {
  return g_hostTransport.url.c_str();
}

const char *honch_arduino_host_transport_last_content_type(void) {
  return g_hostTransport.contentType.c_str();
}

const char *honch_arduino_host_transport_last_project_key(void) {
  return g_hostTransport.projectKey.c_str();
}

const char *honch_arduino_host_transport_last_stream_id(void) {
  return g_hostTransport.streamId.c_str();
}

const uint8_t *honch_arduino_host_transport_last_body(void) {
  return g_hostTransport.body.empty() ? nullptr : g_hostTransport.body.data();
}

size_t honch_arduino_host_transport_last_body_size(void) {
  return g_hostTransport.body.size();
}

void honch_arduino_host_transport_set_result(
    honch_status_t status,
    honch_transport_result_t result) {
  g_hostTransport.status = status;
  g_hostTransport.result = result;
}
#endif
