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
# with backoff (next tick) instead of bricking the device.
#
# TLS: the server certificate is verified against the Google Trust Services
# root R1 (the trust anchor for the default i.honch.io endpoint, matching the
# ESP-IDF and Arduino ports). Verification needs a real clock for the cert
# validity-period check -- sync NTP first (see the MicroPython SDK docs). A
# custom / local endpoint can opt out via verify_tls(False).

import socket
import select
import time

try:  # newer MicroPython exposes the TLS API as `tls`; older as `ssl`
    import tls as _tls
except ImportError:  # pragma: no cover
    import ssl as _tls

# Google Trust Services Root R1 -- the trust anchor i.honch.io's chain terminates
# at (api.honch.io <- WR3 <- GTS Root R1). Same root the ESP-IDF/Arduino ports pin.
_GTS_ROOT_R1 = """\
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
"""

try:
    import binascii as _binascii
except ImportError:  # pragma: no cover
    import ubinascii as _binascii

# mbedtls wants DER unless the firmware was built with PEM-parse support, which is
# not guaranteed on minimal ports -- decode the embedded PEM to DER once at import.
_GTS_ROOT_R1_DER = _binascii.a2b_base64(
    "".join(l for l in _GTS_ROOT_R1.strip().split("\n") if not l.startswith("-----"))
)

# TLS verification on by default (verify against _GTS_ROOT_R1). verify_tls(False)
# opts out for a custom / local endpoint that isn't behind Google Trust Services.
_VERIFY = True


def verify_tls(enabled):
    global _VERIFY
    _VERIFY = bool(enabled)

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
            if _VERIFY:
                ctx.verify_mode = _tls.CERT_REQUIRED
                ctx.load_verify_locations(_GTS_ROOT_R1_DER)
            else:
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
