/* Phase A — honch_error_detail_t struct + reason enum + formatter.
 * Pure header/formatter unit tests; no client, no transport. */
#include <assert.h>
#include <string.h>

#include "honch/core/error_detail.h"
#include "honch/core/honch.h"

/* A1: zero-initialised detail is the "no error" state. */
static void test_struct_zero_is_none(void)
{
    honch_error_detail_t d = {0};
    assert(d.reason == HONCH_REASON_NONE);
    assert(d.http_status == 0 && d.os_error == 0);
    assert(d.message == NULL && d.component == NULL);
}

/* A2: every reason maps to a stable snake_case token; unknown is bounded. */
static void test_reason_string(void)
{
    assert(strcmp(honch_error_reason_string(HONCH_REASON_AUTH_INVALID_KEY), "auth_invalid_key") == 0);
    assert(strcmp(honch_error_reason_string(HONCH_REASON_DNS_FAILED), "dns_failed") == 0);
    assert(strcmp(honch_error_reason_string(HONCH_REASON_QUEUE_FULL), "queue_full") == 0);
    assert(strcmp(honch_error_reason_string(HONCH_REASON_NONE), "none") == 0);
    assert(strcmp(honch_error_reason_string((honch_error_reason_t)9999), "unknown") == 0);
}

/* A2: the formatter carries the coarse status, the HTTP code, the message,
 * and the reason token in one line. */
static void test_format_includes_http_and_reason(void)
{
    honch_error_detail_t d = {0};
    d.status = HONCH_ERROR_REJECTED;
    d.reason = HONCH_REASON_AUTH_INVALID_KEY;
    d.transport_result = HONCH_TRANSPORT_AUTH_ERROR;
    d.http_status = 401;
    d.message = "API key invalid or revoked";

    char buf[200];
    size_t n = honch_error_detail_format(&d, buf, sizeof(buf));
    assert(n > 0 && n < sizeof(buf));
    assert(buf[n] == '\0');
    assert(strstr(buf, "401") != NULL);
    assert(strstr(buf, "API key invalid") != NULL);
    assert(strstr(buf, "auth_invalid_key") != NULL);
}

/* A2: a local (non-HTTP) failure formats without an HTTP code. */
static void test_format_local_failure_no_http(void)
{
    honch_error_detail_t d = {0};
    d.status = HONCH_ERROR_QUEUE_FULL;
    d.reason = HONCH_REASON_QUEUE_FULL;
    d.message = "event queue full, dropping oldest";

    char buf[200];
    size_t n = honch_error_detail_format(&d, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "HTTP") == NULL); /* http_status == 0 -> no HTTP token */
    assert(strstr(buf, "queue_full") != NULL);
}

/* A2: bounded output — never overruns a tiny buffer, always NUL-terminated. */
static void test_format_truncates_safely(void)
{
    honch_error_detail_t d = {0};
    d.status = HONCH_ERROR_TRANSPORT;
    d.reason = HONCH_REASON_HTTP_STATUS;
    d.http_status = 503;
    d.message = "server unavailable";

    char buf[8];
    size_t n = honch_error_detail_format(&d, buf, sizeof(buf));
    assert(buf[sizeof(buf) - 1] == '\0');
    assert(n < sizeof(buf));
}

/* A2: NULL / zero-size guards never write or crash. */
static void test_format_null_guards(void)
{
    char buf[16] = "sentinel";
    assert(honch_error_detail_format(NULL, buf, sizeof(buf)) == 0);
    honch_error_detail_t d = {0};
    assert(honch_error_detail_format(&d, NULL, 0) == 0);
    assert(honch_error_detail_format(&d, buf, 0) == 0);
}

int main(void)
{
    test_struct_zero_is_none();
    test_reason_string();
    test_format_includes_http_and_reason();
    test_format_local_failure_no_http();
    test_format_truncates_safely();
    test_format_null_guards();
    return 0;
}
