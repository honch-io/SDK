"""TLS posture guard for the POSIX libcurl transport.

Honch is secure-by-default (the core endpoint defaults to https://) without
hard-rejecting an explicitly configured http:// endpoint. What must never
regress is the *security of the TLS path*: when a connection is TLS, the
certificate chain and host name are verified, and the transport never speaks an
exotic protocol or follows a redirect into one.

This is a source-level guard (the real curl options are not reachable through the
HONCH_TESTING transport stub used by the host tests). It mirrors the arduino
port's test_tls_config.py and exists to fail loudly if anyone weakens TLS for
local convenience -- a CLAUDE.md non-negotiable.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as handle:
        return handle.read()


class PosixTlsConfigTest(unittest.TestCase):
    def setUp(self):
        self.src = read("ports/posix/src/posix_transport_curl.c")
        self.normalized = " ".join(self.src.split())

    def test_peer_verification_is_pinned_on(self):
        self.assertIn("CURLOPT_SSL_VERIFYPEER, 1L", self.normalized)

    def test_host_verification_is_pinned_on(self):
        self.assertIn("CURLOPT_SSL_VERIFYHOST, 2L", self.normalized)

    def test_verification_is_never_disabled(self):
        # No path may set peer/host verification to 0 (the weakening footgun).
        self.assertNotRegex(self.normalized, r"CURLOPT_SSL_VERIFYPEER,\s*0")
        self.assertNotRegex(self.normalized, r"CURLOPT_SSL_VERIFYHOST,\s*0")

    def test_protocols_restricted_to_http_and_https(self):
        # Either the modern string form or the legacy bitmask, but present.
        restricted = (
            'CURLOPT_PROTOCOLS_STR, "http,https"' in self.normalized
            or re.search(r"CURLOPT_PROTOCOLS,\s*\(long\)\(CURLPROTO_HTTP \| CURLPROTO_HTTPS\)", self.normalized)
        )
        self.assertTrue(restricted, "transport must restrict protocols to http/https")

    def test_redirects_are_not_enabled(self):
        # Following redirects could smuggle the project-key header to another
        # host/scheme, so FOLLOWLOCATION must never be turned on.
        self.assertNotRegex(self.normalized, r"CURLOPT_FOLLOWLOCATION,\s*1")


if __name__ == "__main__":
    unittest.main()
