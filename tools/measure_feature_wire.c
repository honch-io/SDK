/* Measure the on-the-wire (wire-v2) encoded size of each feature's auto-emitted
 * events. The number we publish to the wizard is the *marginal* bytes one such
 * event adds to an upload batch — i.e. the ongoing network cost per event, with
 * the per-batch device context (sent once) amortized out.
 *
 * This uses the real wire encoder via honch_wire_v2_measure_event_batch, with
 * each event's actual property set (read from the emit paths in core/src). Wire
 * encoding is portable, so this runs on any host — no device or ESP-IDF needed:
 *
 *   cc -std=c11 -I core/include -I core/src \
 *      tools/measure_feature_wire.c core/src/honch_wire_v2.c -o /tmp/measure_wire
 *   /tmp/measure_wire
 */
#include "honch/core/wire_v2.h"

#include <stdio.h>
#include <stdlib.h>

/* A representative batch context — the device-level fields sent once per upload.
 * These do NOT vary per feature; the marginal-per-event number cancels them. */
static honch_wire_v2_batch_context_t representative_context(void) {
    honch_wire_v2_batch_context_t ctx = {0};
    ctx.distinct_id = "dev_a3f2c1d4e5f6";
    ctx.device_id = "dev_a3f2c1d4e5f6";
    ctx.device_model = "X3-Pro";
    ctx.firmware_version = "3.4.1";
    ctx.sdk_platform = "esp-idf";
    ctx.sdk_version = "0.2.5";
    ctx.environment = "production";
    ctx.session_id = "sess_5f4e3d2c1b0a";
    return ctx;
}

#define BASE_MS 1700000000000ULL

/* Full encoded size of one event on the wire: a batch carrying just this event
 * minus the same batch carrying none. This counts the event's full cost — name
 * string + timestamp + every property key and value — with no cross-event
 * string-table dedup (each event measured as if its strings are new). That is
 * the honest standalone "bytes per event" and a safe upper bound: when many
 * events batch together, shared strings dedup and the real per-event cost only
 * drops. */
static long event_wire_bytes(const char *name,
                             const honch_wire_v2_property_t *props,
                             size_t prop_count) {
    honch_wire_v2_batch_context_t ctx = representative_context();
    /* The encoder requires >=1 event, so we can't baseline on an empty batch.
     * Instead use a fixed, property-free anchor event in both measurements; its
     * strings don't overlap the target's, so (anchor+target) - (anchor) is the
     * target's full standalone encoding (name + timestamp + keys + values). */
    honch_wire_v2_event_t anchor[1] = {{"_", BASE_MS + 1000u, NULL, 0}};
    honch_wire_v2_event_t both[2] = {
        {"_", BASE_MS + 1000u, NULL, 0},
        {name, BASE_MS + 2000u, props, prop_count},
    };
    size_t ba = 0, bb = 0;
    if (honch_wire_v2_measure_event_batch(&ctx, BASE_MS, anchor, 1, &ba) != HONCH_OK ||
        honch_wire_v2_measure_event_batch(&ctx, BASE_MS, both, 2, &bb) != HONCH_OK) {
        fprintf(stderr, "measure failed for %s\n", name);
        exit(1);
    }
    return (long)bb - (long)ba;
}

int main(void) {
    /* Per-event property sets, mirroring the emit paths in core/src
     * (honch_lifecycle_events.c, honch_sessions.c, honch_battery.c,
     * honch_crash.c). */
    const honch_wire_v2_property_t boot[] = {
        honch_prop("reset_reason", honch_str("power_on")),
    };
    const honch_wire_v2_property_t fw_update[] = {
        honch_prop("previous_version", honch_str("3.4.0")),
        honch_prop("new_version", honch_str("3.4.1")),
    };
    const honch_wire_v2_property_t session_start[] = {
        honch_prop("session_name", honch_str("recording")),
    };
    const honch_wire_v2_property_t battery_low[] = {
        honch_prop("level", honch_i64(12)),
    };
    const honch_wire_v2_property_t crash[] = {
        honch_prop("source", honch_str("hardfault")),
        honch_prop("severity", honch_str("fatal")),
        honch_prop("reset_reason", honch_str("panic")),
        honch_prop("summary_version", honch_u64(1)),
        honch_prop("message", honch_str("Guru Meditation Error: Core 0 panic'd (LoadProhibited)")),
        honch_prop("fault_pc", honch_str("0x400d1234")),
    };
    const honch_wire_v2_property_t error_log[] = {
        honch_prop("component", honch_str("wifi")),
        honch_prop("message", honch_str("connection timeout after 5000ms")),
        honch_prop("count", honch_u64(3)),
    };

    struct {
        const char *feature;
        const char *event;
        const honch_wire_v2_property_t *props;
        size_t count;
        int representative; /* the headline event shown in the wizard picker */
    } events[] = {
        {"lifecycle", "$device_boot", boot, 1, 1},
        {"lifecycle", "$firmware_update", fw_update, 2, 0},
        {"lifecycle", "$device_shutdown", NULL, 0, 0},
        {"sessions", "$session_start", session_start, 1, 1},
        {"sessions", "$session_end", NULL, 0, 0},
        {"battery", "$battery_low", battery_low, 1, 1},
        {"error-tracking", "$crash", crash, 6, 1},
        {"error-tracking", "$error", error_log, 3, 0},
    };

    printf("{\n  \"unit\": \"bytes per event, full standalone wire-v2 encoding (no cross-event string dedup; upper bound)\",\n");
    printf("  \"events\": [\n");
    size_t n = sizeof(events) / sizeof(events[0]);
    for (size_t i = 0; i < n; i++) {
        long bytes = event_wire_bytes(events[i].event, events[i].props, events[i].count);
        printf("    {\"feature\": \"%s\", \"event\": \"%s\", \"wire_bytes\": %ld, \"representative\": %s}%s\n",
               events[i].feature, events[i].event, bytes,
               events[i].representative ? "true" : "false",
               (i + 1 < n) ? "," : "");
    }
    printf("  ]\n}\n");
    return 0;
}
