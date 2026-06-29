#include "honch_arduino_adapter.h"
#include "honch/core/capture_transport.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#ifdef ARDUINO
#include <HTTPClient.h>
#include <new>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "honch_gts_roots.h"
#endif

#define HONCH_ARDUINO_DEFAULT_TRANSPORT_TIMEOUT_MS 8000u
#define HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS 10000u

namespace {

#ifndef ARDUINO
struct HostTransportState {
  honch_status_t status = HONCH_OK;
  honch_transport_result_t result = HONCH_TRANSPORT_ACCEPTED;
  int httpStatus = 0; /* host-test seam: drives post_chunk_ex detail.http_status */
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

honch_status_t classify_http_status(int code, honch_transport_result_t *result) {
  if (result == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  if (code == 202) {
    *result = HONCH_TRANSPORT_CHUNK_STORED;
    return HONCH_OK;
  }
  if (code == 204) {
    *result = HONCH_TRANSPORT_ACCEPTED;
    return HONCH_OK;
  }
  if (code == 401) {
    *result = HONCH_TRANSPORT_AUTH_ERROR;
    return HONCH_ERROR_REJECTED;
  }
  if (code == 408) {
    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_ERROR_TIMEOUT;
  }
  if (code == 409) {
    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_ERROR_TRANSPORT;
  }
  if (code == 429) {
    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_ERROR_RATE_LIMITED;
  }
  if (code >= 500) {
    *result = HONCH_TRANSPORT_RETRY;
    return HONCH_ERROR_SERVER;
  }
  if (code >= 400) {
    *result = HONCH_TRANSPORT_REJECTED;
    return HONCH_ERROR_REJECTED;
  }
  *result = HONCH_TRANSPORT_RETRY;
  return HONCH_ERROR_TRANSPORT;
}

#ifdef ARDUINO
// The default Honch endpoint (https://i.honch.io) is behind Google Trust
// Services; pin the GTS roots for it (a device's CA set may lack the legacy root
// its chain anchors at). Exact-host match so look-alikes (i.honch.io.evil.com)
// don't get the pinned trust. Custom/BYO endpoints keep using rootCaPem.
static bool arduino_endpoint_is_default(const char *endpointUrl) {
  static const char prefix[] = "https://i.honch.io";
  const size_t prefix_len = sizeof(prefix) - 1;
  if (endpointUrl == nullptr || strncmp(endpointUrl, prefix, prefix_len) != 0) {
    return false;
  }
  char next = endpointUrl[prefix_len];
  return next == '\0' || next == '/' || next == ':';
}
#endif

#ifdef ARDUINO
/* Map a negative HTTPClient transport error (no HTTP status) to a finer reason. */
static honch_error_reason_t arduino_map_httpc_error(int code) {
  switch (code) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
      return HONCH_REASON_CONNECT_REFUSED;
    case HTTPC_ERROR_CONNECTION_LOST:
      return HONCH_REASON_READ_FAILED;
    case HTTPC_ERROR_READ_TIMEOUT:
      return HONCH_REASON_CONNECT_TIMEOUT;
    case HTTPC_ERROR_SEND_HEADER_FAILED:
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
      return HONCH_REASON_WRITE_FAILED;
    case HTTPC_ERROR_NOT_CONNECTED:
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return HONCH_REASON_OFFLINE;
    default:
      return HONCH_REASON_UNKNOWN;
  }
}
#endif

honch_status_t arduino_post_chunk_ex(
    void *ctx,
    const char *endpointUrl,
    const char *apiKey,
    const char *streamId,
    const uint8_t *body,
    size_t bodySize,
    honch_transport_result_t *result,
    honch_transport_detail_t *detail) {
  honch_arduino_transport_t *transport = static_cast<honch_arduino_transport_t *>(ctx);
  if (transport == nullptr || endpointUrl == nullptr || apiKey == nullptr || body == nullptr ||
      bodySize == 0 || result == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  std::string url = capture_url(endpointUrl);
  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure *secureClient = nullptr;
  bool https = url.rfind("https://", 0) == 0;
  if (https) {
    secureClient = new (std::nothrow) WiFiClientSecure();
    if (secureClient == nullptr) {
      *result = HONCH_TRANSPORT_RETRY;
      if (detail != nullptr) {
        detail->reason = HONCH_REASON_OUT_OF_MEMORY;
      }
      return HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (transport->rootCaPem != nullptr && transport->rootCaPem[0] != '\0') {
      secureClient->setCACert(transport->rootCaPem);
    } else if (transport->insecureSkipTlsVerify) {
      log_w("Honch: TLS certificate verification disabled (insecureSkipTlsVerify); not for production use");
      secureClient->setInsecure();
    } else if (arduino_endpoint_is_default(endpointUrl)) {
      // Default Honch endpoint: trust the Google Trust Services roots directly,
      // so it verifies without the integrator having to supply a rootCaPem.
      secureClient->setCACert(HONCH_GTS_ROOTS_PEM);
    }
    if (!http.begin(*secureClient, url.c_str())) {
      delete secureClient;
      *result = HONCH_TRANSPORT_RETRY;
      if (detail != nullptr) {
        detail->reason = HONCH_REASON_TLS_HANDSHAKE;
      }
      return HONCH_ERROR_TRANSPORT;
    }
  } else {
    if (!http.begin(plainClient, url.c_str())) {
      *result = HONCH_TRANSPORT_RETRY;
      if (detail != nullptr) {
        detail->reason = HONCH_REASON_CONNECT_REFUSED;
      }
      return HONCH_ERROR_TRANSPORT;
    }
  }

  http.setTimeout(transport->transportTimeoutMs);
  http.addHeader("Content-Type", "application/vnd.honch.chunk");
  http.addHeader("X-Honch-Project-Key", apiKey);
  if (streamId != nullptr && streamId[0] != '\0') {
    http.addHeader("X-Honch-Stream-Id", streamId);
  }

  int code = http.POST(const_cast<uint8_t *>(body), bodySize);
  http.end();
  delete secureClient;
  if (detail != nullptr) {
    if (code > 0) {
      detail->http_status = code;
      detail->reason = honch_capture_map_http_reason(code);
    } else {
      detail->os_error = code;
      detail->reason = arduino_map_httpc_error(code);
    }
  }
  return classify_http_status(code, result);
#else
  g_hostTransport.calls++;
  g_hostTransport.url = capture_url(endpointUrl);
  g_hostTransport.contentType = "application/vnd.honch.chunk";
  g_hostTransport.projectKey = apiKey;
  g_hostTransport.streamId = streamId == nullptr ? "" : streamId;
  g_hostTransport.body.assign(body, body + bodySize);
  *result = g_hostTransport.result;
  if (detail != nullptr) {
    detail->http_status = g_hostTransport.httpStatus;
    detail->reason = g_hostTransport.httpStatus > 0
                         ? honch_capture_map_http_reason(g_hostTransport.httpStatus)
                         : HONCH_REASON_NONE;
  }
  return g_hostTransport.status;
#endif
}

honch_status_t arduino_post_chunk(
    void *ctx,
    const char *endpointUrl,
    const char *apiKey,
    const char *streamId,
    const uint8_t *body,
    size_t bodySize,
    honch_transport_result_t *result) {
  return arduino_post_chunk_ex(ctx, endpointUrl, apiKey, streamId, body, bodySize, result, nullptr);
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
  ctx->rootCaPem = config.rootCaPem;
  ctx->transportTimeoutMs = config.transportTimeoutMs == 0u ?
      HONCH_ARDUINO_DEFAULT_TRANSPORT_TIMEOUT_MS :
      config.transportTimeoutMs;
  if (ctx->transportTimeoutMs > HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS) {
    ctx->transportTimeoutMs = HONCH_ARDUINO_MAX_TRANSPORT_TIMEOUT_MS;
  }
  ctx->insecureSkipTlsVerify = config.insecureSkipTlsVerify;
  *ops = honch_transport_ops_t{
      arduino_post_chunk,
      nullptr,
      ctx,
      arduino_post_chunk_ex,
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

void honch_arduino_host_transport_set_http_status(int httpStatus) {
  g_hostTransport.httpStatus = httpStatus;
}

honch_status_t honch_arduino_host_classify_http_status(
    int code,
    honch_transport_result_t *result) {
  return classify_http_status(code, result);
}
#endif
