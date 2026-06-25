/* Acceptance test for the HONCH_ENABLE_BATTERY compile toggle.
 *
 * Battery telemetry is woven through the event hot path (the $battery_level
 * auto-property is sampled per event and the $battery_low edge event fires from
 * the track path), and the suite's battery tests share helpers, so — like the
 * lifecycle toggle — the off-config is covered by this dedicated test rather
 * than by forcing the whole suite through it. The full suite stays battery-ON.
 *
 * Decode-free off-proof: a counting battery_callback. With the feature ON the
 * core samples it once per event; with it OFF honch_read_battery_level is gated
 * to return -1 without ever invoking the callback, so the count stays 0 — i.e.
 * the sampling (and therefore $battery_level / $battery_low) is compiled out.
 *
 * Run on-branch as part of the default suite; run off-branch from a tree
 * configured with -DHONCH_ENABLE_BATTERY=0.
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

static int g_battery_invocations = 0;

static int counting_battery_callback(void)
{
    g_battery_invocations++;
    return 50; /* above any threshold; value is irrelevant to this test */
}

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
    snprintf(queue_dir, sizeof queue_dir, "/tmp/honch-battery-toggle-%ld-XXXXXX", (long)getpid());
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
        .flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS,
        .battery_callback = counting_battery_callback,
        .battery_low_threshold = 20
    };

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "probe_event", NULL, 0u), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

#if HONCH_ENABLE_BATTERY
    /* Battery ON: the core samples the callback once per event (boot + track). */
    EXPECT_TRUE(g_battery_invocations >= 1);
#else
    /* Battery OFF: the sampler is compiled out, so the callback is never invoked
     * even though config.battery_callback is set — no $battery_level/$battery_low
     * can be produced. The SDK still works for normal events (flush succeeded). */
    EXPECT_EQ_INT(g_battery_invocations, 0);
    EXPECT_TRUE(transport.calls >= 1);
#endif

    EXPECT_EQ_INT(honch_shutdown(client), HONCH_OK);
    honch_test_set_transport(NULL, NULL);

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("battery toggle test passed\n");
    return 0;
}
