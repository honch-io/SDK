/*
 * Honch automatic error & crash reporting demo (POSIX).
 *
 * This program PROVES the two automatic capture paths by deliberately failing:
 *
 *   ./honch_crash_demo crash     Logs an error ($error) and then dereferences
 *                                NULL, crashing with SIGSEGV. The Honch signal
 *                                handler writes an async-signal-safe breadcrumb.
 *
 *   ./honch_crash_demo recover   The next run: honch_init() imports the
 *                                breadcrumb and queues a $crash, which is
 *                                flushed; erase-after-ack then clears the
 *                                breadcrumb so it is not re-reported.
 *
 * Neither path uses any manual error API — capture is automatic.
 */
#include "honch/honch.h"

#include <stdio.h>
#include <string.h>

static honch_config_t demo_config(void)
{
    honch_config_t config = {
        .api_key = "local-dev-key",
        .endpoint_url = "http://127.0.0.1:8765",
        .device_id = "crash-demo-device",
        .device_model = "Crash Demo Rig",
        .firmware_version = "1.0.0",
        .environment = "dev",
        .queue_directory = ".honch-crash-demo",
        .batch_size = 10u,
        .max_queued_events = 100u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 2000u
    };
    return config;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "crash";
    honch_config_t config = demo_config();

    /* Opt into fatal-signal capture. The handler only writes a bounded
     * breadcrumb with async-signal-safe syscalls; the NEXT honch_init() turns it
     * into a $crash. Installed before init so init can import a prior breadcrumb. */
    if (honch_install_error_handlers(config.queue_directory) != HONCH_OK) {
        fprintf(stderr, "honch_install_error_handlers failed\n");
        return 1;
    }

    honch_client_t *client = NULL;
    if (honch_init(&client, &config) != HONCH_OK) {
        fprintf(stderr, "honch_init failed\n");
        return 1;
    }

    if (strcmp(mode, "recover") == 0) {
        /* honch_init() already imported the previous run's crash breadcrumb and
         * queued a $crash. Flush delivers it; erase-after-ack clears the source. */
        printf("recover: flushing recovered $crash...\n");
        honch_status_t status = honch_flush(client);
        printf("recover: flush status = %s\n", honch_status_string(status));
        honch_shutdown(client);
        return 0;
    }

    /* crash mode: automatic logged-error capture, then a deliberate fault. */
    printf("crash: emitting an automatic $error, then crashing on purpose...\n");
    honch_core_report_log_error(client, "sensor", "temperature read failed: bus timeout");
    honch_flush(client); /* deliver the $error before we die */

    /* Deliberate NULL dereference -> SIGSEGV -> Honch breadcrumb handler fires. */
    volatile int *boom = NULL;
    *boom = 42;

    honch_shutdown(client); /* unreachable */
    return 0;
}
