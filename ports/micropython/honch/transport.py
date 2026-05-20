try:
    import urequests as requests
except ImportError:
    requests = None

from .errors import (
    RateLimitedError,
    RejectedError,
    ServerError,
    TransportError,
)


class HttpTransport:
    def post(self, url, body, headers, timeout_ms):
        if requests is None:
            raise TransportError("no HTTP transport available")
        try:
            response = requests.post(url, data=body, headers=headers, timeout=timeout_ms / 1000.0)
        except TypeError as exc:
            if not _timeout_keyword_unsupported(exc):
                raise
            response = requests.post(url, data=body, headers=headers)
        try:
            return response.status_code
        finally:
            close = getattr(response, "close", None)
            if close is not None:
                close()


def _timeout_keyword_unsupported(exc):
    message = str(exc).lower()
    return "timeout" in message and "keyword" in message


def batch_url(endpoint_url):
    while endpoint_url.endswith("/"):
        endpoint_url = endpoint_url[:-1]
    return endpoint_url + "/batch"


def post_batch(config, platform, transport, payload):
    raw = payload
    compressed = None
    if not config.disable_gzip and len(raw) >= config.gzip_min_bytes:
        try:
            candidate = platform.gzip_compress(raw)
            if candidate is not None and len(candidate) < len(raw):
                compressed = candidate
        except Exception:
            compressed = None

    if compressed is None:
        body = raw
        headers = {"Content-Type": "application/cbor"}
    else:
        body = compressed
        headers = {
            "Content-Type": "application/cbor",
            "Content-Encoding": "gzip",
        }

    status = transport.post(
        batch_url(config.endpoint_url),
        body,
        headers,
        config.transport_timeout_ms,
    )
    if 200 <= status < 300:
        return
    if status == 429:
        raise RateLimitedError("rate limited")
    if status >= 500:
        raise ServerError("server error")
    raise RejectedError("batch rejected")
