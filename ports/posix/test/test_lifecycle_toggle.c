/* Acceptance test for the HONCH_ENABLE_LIFECYCLE_EVENTS compile toggle.
 *
 * The main POSIX suite asserts the default (lifecycle-ON) product, where
 * $device_boot/$firmware_update are ambient queue traffic. This dedicated test
 * pins the toggle contract in BOTH compile configurations, counting outbound
 * transport calls rather than decoding payloads:
 *
 *   ON  (default): honch_init auto-emits $device_boot (+ $firmware_update on a
 *                  version change), so the first flush sends at least one batch.
 *   OFF          : honch_init emits no lifecycle events, so the first flush has
 *                  nothing to send; the SDK still works for explicit track().
 *
 * Run on-branch as part of the default suite; run off-branch from a tree
 * configured with -DHONCH_ENABLE_LIFECYCLE_EVENTS=0 (the full suite is not
 * designed to pass with lifecycle stripped — this targeted test is).
 */
#define _POSIX_C_SOURCE 200809L

#include "honch/honch.h"
#include "honch/posix/honch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define EXPECT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)
#define EXPECT_EQ_INT(a, b) do { int av = (int)(a); int bv = (int)(b); if (av != bv) { fprintf(stderr, "FAIL %s:%d: %s == %s (%d != %d)\n", __FILE__, __LINE__, #a, #b, av, bv); failures++; } } while (0)

typedef struct {
    int calls;
} fake_transport_t;

static honch_status_t fake_transport(
    const char *url,
    const char *api_key,
    const char *stream_id,
    const unsigned char *body,
    size_t body_size,
    const char *content_encoding,
    void *userdata,
    long *http_status)
{
    (void)url;
    (void)api_key;
    (void)stream_id;
    (void)body;
    (void)body_size;
    (void)content_encoding;
    fake_transport_t *transport = (fake_transport_t *)userdata;
    transport->calls++;
    *http_status = 204L;
    return HONCH_OK;
}

int main(void)
{
    char queue_dir[256];
    snprintf(queue_dir, sizeof queue_dir, "/tmp/honch-life-toggle-%ld-XXXXXX", (long)getpid());
    EXPECT_TRUE(mkdtemp(queue_dir) != NULL);

    fake_transport_t transport = {0};
    honch_test_set_transport(fake_transport, &transport);

    honch_config_t config = {
        .api_key = "test-key",
        .endpoint_url = "http://collector.local/",
        .device_id = "device-1",
        .device_model = "X3-Pro",
        .firmware_version = "1.0.0",
        .environment = "production",
        .queue_directory = queue_dir,
        .batch_size = 10u,
        .max_queued_events = 50u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 1000u,
        .flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS
    };

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

#if HONCH_ENABLE_LIFECYCLE_EVENTS
    /* Lifecycle ON: init auto-emitted at least $device_boot, so the first flush
     * shipped a batch. */
    EXPECT_TRUE(transport.calls >= 1);
#else
    /* Lifecycle OFF: init emitted no $device_boot/$firmware_update, so the first
     * flush had nothing to send. The SDK still functions for explicit events. */
    EXPECT_EQ_INT(transport.calls, 0);
    EXPECT_EQ_INT(honch_track(client, "toggle_probe", NULL, 0u), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_EQ_INT(transport.calls, 1);
#endif

    EXPECT_EQ_INT(honch_shutdown(client), HONCH_OK);
    honch_test_set_transport(NULL, NULL);

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("lifecycle toggle test passed\n");
    return 0;
}
