# Fully time-bounded HTTP(S) chunk POST for constrained, single-threaded
# MicroPython ports (rp2 / Pico W).
#
# Why this exists: micropython-lib `requests`/`urequests` calls socket.settimeout()
# but on rp2 (lwIP + mbedtls) that does NOT bound socket.connect() OR the TLS
# handshake OR blocking reads on a flaky / transitional Wi-Fi link. The op then
# blocks forever and, because the port is single-threaded, the whole VM (and the
# USB-CDC service) is parked -- the device hard-wedges and no event is delivered.
#
# This helper runs the ENTIRE exchange on a NON-BLOCKING socket driven by
# select.poll() against a single hard deadline: connect, the TLS handshake
# (deferred via do_handshake_on_connect=False and driven by the first write),
# the request send, and the status read are each bounded. A stalled link raises
# OSError promptly; the Honch C core maps that to TRANSPORT/TIMEOUT and retries
# with backoff (next tick) instead of bricking the device. TLS posture is
# unchanged (CERT_NONE), behaviour on a healthy link is identical.

import socket
import select
import time

try:  # newer MicroPython exposes the TLS API as `tls`; older as `ssl`
    import tls as _tls
except ImportError:  # pragma: no cover
    import ssl as _tls

# connect() on a non-blocking socket reports "in progress" via these errnos
_INPROGRESS = (115, 119, 36, 11, 10035, 10036)
_EAGAIN = (11, 35, 10035)  # would-block on non-blocking read/write


def _deadline_poll(poller, deadline):
    """Block until the socket is ready or the deadline passes; raise on timeout."""
    rem = time.ticks_diff(deadline, time.ticks_ms())
    if rem <= 0:
        raise OSError("honch: timeout")
    # poll() honors the ssl socket's internal poll_mask (read- vs write-ready)
    if not poller.poll(rem):
        raise OSError("honch: timeout")


def post_chunk(url, body, headers, timeout_ms):
    """POST `body` to `url`; return the integer HTTP status. Raises OSError if it
    cannot complete within timeout_ms (connect / handshake / send / recv)."""
    if not timeout_ms or timeout_ms <= 0:
        timeout_ms = 8000

    proto, _, hostport, path = url.split("/", 3)
    https = proto == "https:"
    if ":" in hostport:
        host, _, port_s = hostport.partition(":")
        port = int(port_s)
    else:
        host = hostport
        port = 443 if https else 80

    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    ai = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)[0]
    s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])
    poller = select.poll()
    try:
        # --- non-blocking connect (the step a flaky link hangs on) ---
        s.setblocking(False)
        try:
            s.connect(ai[-1])
        except OSError as e:
            if e.args[0] not in _INPROGRESS:
                raise
        poller.register(s, select.POLLOUT)
        _deadline_poll(poller, deadline)
        poller.unregister(s)

        # --- TLS: defer the handshake; the first write drives it, bounded below ---
        if https:
            ctx = _tls.SSLContext(_tls.PROTOCOL_TLS_CLIENT)
            ctx.verify_mode = _tls.CERT_NONE
            s = ctx.wrap_socket(s, server_hostname=host, do_handshake_on_connect=False)
            s.setblocking(False)

        poller.register(s, select.POLLIN | select.POLLOUT)

        # --- send request (handshake completes transparently on first write) ---
        head = "POST /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n" % (path, host)
        for k in headers:
            head += "%s: %s\r\n" % (k, headers[k])
        head += "Content-Length: %d\r\n\r\n" % len(body)
        data = head.encode() + bytes(body)
        mv = memoryview(data)
        while mv:
            try:
                n = s.write(mv)
            except OSError as e:
                if e.args[0] not in _EAGAIN:
                    raise
                n = None
            if not n:  # None or 0 -> would block
                _deadline_poll(poller, deadline)
                continue
            mv = mv[n:]

        # --- read just the status line ---
        buf = b""
        while buf.find(b"\n") < 0:
            try:
                chunk = s.read(96)
            except OSError as e:
                if e.args[0] not in _EAGAIN:
                    raise
                chunk = None
            if chunk is None:
                _deadline_poll(poller, deadline)
                continue
            if not chunk:  # clean EOF
                break
            buf += chunk
        try:
            return int(buf.split(None, 2)[1])
        except (IndexError, ValueError):
            return 0
    finally:
        try:
            s.close()
        except Exception:
            pass
