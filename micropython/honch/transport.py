try:
    import urequests as requests
except ImportError:
    requests = None

from .errors import (
    CompressionUnavailableError,
    RateLimitedError,
    RejectedError,
    ServerError,
    TransportError,
)


class HttpTransport:
    def post(self, url, body, headers, timeout_ms):
        if requests is None:
            raise TransportError("no HTTP transport available")
        response = requests.post(url, data=body, headers=headers)
        try:
            return response.status_code
        finally:
            close = getattr(response, "close", None)
            if close is not None:
                close()


def batch_url(endpoint_url):
    while endpoint_url.endswith("/"):
        endpoint_url = endpoint_url[:-1]
    return endpoint_url + "/batch"


def post_batch(config, platform, transport, payload):
    compressed = platform.gzip_compress(payload.encode("utf-8"))
    if compressed is None:
        raise CompressionUnavailableError("gzip support is unavailable")
    status = transport.post(
        batch_url(config.endpoint_url),
        compressed,
        {
            "Content-Type": "application/json",
            "Content-Encoding": "gzip",
        },
        config.transport_timeout_ms,
    )
    if 200 <= status < 300:
        return
    if status == 429:
        raise RateLimitedError("rate limited")
    if status >= 500:
        raise ServerError("server error")
    raise RejectedError("batch rejected")
