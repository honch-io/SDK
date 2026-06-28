#include "honch/honch.h"
#include "honch/posix/honch.h"
#include "honch_internal.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Single global breadcrumb path: the POSIX crash-breadcrumb handler is
 * process-global and therefore single-client. A process that runs more than one
 * honch client concurrently would share this one path; key it per client if
 * multi-client support is ever needed.
 */
static char s_error_breadcrumb_path[512];

static const char *honch_posix_signal_code(int signum)
{
    switch (signum) {
    case SIGABRT:
        return "SIGABRT";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "SIGNAL";
    }
}

static void honch_posix_signal_handler(int signum)
{
    int fd = open(s_error_breadcrumb_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        char message[32];
        size_t used = 0u;
        const char prefix[] = "signal:";
        for (size_t i = 0u; i < sizeof(prefix) - 1u; i++) {
            message[used++] = prefix[i];
        }
        unsigned int value = signum < 0 ? 0u : (unsigned int)signum;
        char digits[10];
        size_t digit_count = 0u;
        do {
            digits[digit_count++] = (char)('0' + (value % 10u));
            value /= 10u;
        } while (value > 0u && digit_count < sizeof(digits));
        while (digit_count > 0u && used < sizeof(message) - 1u) {
            message[used++] = digits[--digit_count];
        }
        message[used++] = '\n';
        (void)write(fd, message, used);
        (void)close(fd);
    }
    (void)signal(signum, SIG_DFL);
    (void)raise(signum);
}

static honch_status_t honch_posix_import_error_breadcrumb(honch_client_t *client)
{
    if (client == NULL || s_error_breadcrumb_path[0] == '\0') {
        return HONCH_OK;
    }
    FILE *file = fopen(s_error_breadcrumb_path, "r");
    if (file == NULL) {
        return HONCH_OK;
    }
    int signum = 0;
    int matched = fscanf(file, "signal:%d", &signum);
    (void)fclose(file);
    if (matched != 1 || signum <= 0) {
        /* Useless breadcrumb: drop it now so it is not re-read forever. */
        (void)unlink(s_error_breadcrumb_path);
        return HONCH_OK;
    }

    /* Do NOT unlink here: the breadcrumb is the crash source and is cleared only
     * after the $crash is delivered, via honch_posix_crash_uploaded (erase-after
     * -ack). If delivery never happens this run, the next run re-reports it. */
    const char *signal_name = honch_posix_signal_code(signum);
    honch_crash_report_t report = {
        .kind = HONCH_CRASH_KIND_SIGNAL,
        .severity = HONCH_CRASH_SEVERITY_FATAL,
        .reset_reason = signal_name,
        .message = "process terminated by signal",
        .component = "process",
        .exception_cause = signal_name
    };
    return honch_core_report_crash(client, &report);
}

/* Erase-after-ack: clear the signal breadcrumb once its $crash has reached the
 * collector, so a crash is neither lost (if the process dies before delivery)
 * nor re-reported on every subsequent run. */
static void honch_posix_crash_uploaded(void *userdata)
{
    (void)userdata;
    if (s_error_breadcrumb_path[0] != '\0') {
        (void)unlink(s_error_breadcrumb_path);
    }
}

static honch_status_t honch_posix_install_signal_handler(int signum)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = honch_posix_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    return sigaction(signum, &action, NULL) == 0 ? HONCH_OK : HONCH_ERROR_IO;
}

static void honch_posix_config_to_core(const honch_config_t *config, honch_core_config_t *core_config)
{
    *core_config = (honch_core_config_t) {
        .api_key = config->api_key,
        .endpoint_url = config->endpoint_url,
        .device_id = config->device_id,
        .device_model = config->device_model,
        .firmware_version = config->firmware_version,
        .environment = config->environment,
        .sdk_platform = "c-posix",
        .queue_directory = config->queue_directory,
        .batch_size = config->batch_size,
        .max_queued_events = config->max_queued_events,
        .max_event_bytes = config->max_event_bytes,
        .transport_timeout_ms = config->transport_timeout_ms,
        .flush_interval_seconds = config->flush_interval_seconds,
        .flush_min_interval_ms = config->flush_min_interval_ms,
        .flush_event_threshold = config->flush_event_threshold,
        .flush_max_batches = (size_t)-1,
        .shutdown_flush_max_batches = (size_t)-1,
        .flush_retry_initial_ms = config->flush_retry_initial_ms,
        .flush_retry_max_ms = config->flush_retry_max_ms,
        .battery_callback = config->battery_callback,
        .battery_low_threshold = config->battery_low_threshold,
        .auto_properties_callback = config->auto_properties_callback,
        .auto_properties_userdata = config->auto_properties_userdata,
        .connectivity_callback = config->connectivity_callback,
        .connectivity_userdata = config->connectivity_userdata,
        .durability_mode = config->durability_mode
    };
}

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config)
{
    if (config == NULL) {
        return honch_core_init(client, NULL);
    }
    if (config->durability_mode != HONCH_DURABILITY_SYNC_ALWAYS &&
        config->durability_mode != HONCH_DURABILITY_OS_BUFFERED) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_platform_ops_t platform_ops;
    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t queue_ops;
    honch_transport_ops_t transport_ops;
    honch_posix_platform_t platform_ctx;
    honch_posix_storage_t storage_ctx;
    honch_posix_transport_t *transport_ctx = NULL;

    honch_status_t status = honch_posix_platform_ops_init(&platform_ops, &platform_ctx);
    if (status == HONCH_OK) {
        status = honch_posix_storage_ops_init(&state_ops, &queue_ops, &storage_ctx, config->queue_directory);
    }
    if (status == HONCH_OK) {
        transport_ctx = (honch_posix_transport_t *)calloc(1u, sizeof(*transport_ctx));
        if (transport_ctx == NULL) {
            status = HONCH_ERROR_OUT_OF_MEMORY;
        }
    }
    if (status == HONCH_OK) {
        status = honch_posix_transport_ops_init(&transport_ops, transport_ctx);
    }
    if (status != HONCH_OK) {
        if (transport_ctx != NULL) {
            honch_posix_transport_ops_deinit(transport_ctx);
            free(transport_ctx);
        }
        return status;
    }

    honch_core_config_t core_config;
    honch_posix_config_to_core(config, &core_config);
    core_config.platform = &platform_ops;
    core_config.state_storage = &state_ops;
    core_config.event_queue = &queue_ops;
    core_config.transport = &transport_ops;
    core_config.crash_uploaded_callback = honch_posix_crash_uploaded;
    status = honch_core_init(client, &core_config);
    if (status == HONCH_OK) {
        transport_ctx->client = *client;
        (void)honch_posix_import_error_breadcrumb(*client);
    } else {
        honch_posix_transport_ops_deinit(transport_ctx);
        free(transport_ctx);
    }
    return status;
}

honch_status_t honch_track(
    honch_client_t *client,
    const char *event_name,
    const honch_property_t *properties,
    size_t property_count)
{
    return honch_core_track(client, event_name, properties, property_count);
}

honch_status_t honch_identify(
    honch_client_t *client,
    const char *distinct_id,
    const honch_property_t *traits,
    size_t trait_count)
{
    return honch_core_identify(client, distinct_id, traits, trait_count);
}

honch_status_t honch_install_error_handlers(const char *queue_directory)
{
    if (queue_directory == NULL || queue_directory[0] == '\0') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    int written = snprintf(
        s_error_breadcrumb_path,
        sizeof(s_error_breadcrumb_path),
        "%s/runtime-error.breadcrumb",
        queue_directory);
    if (written <= 0 || (size_t)written >= sizeof(s_error_breadcrumb_path)) {
        s_error_breadcrumb_path[0] = '\0';
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = honch_posix_install_signal_handler(SIGABRT);
    if (status == HONCH_OK) {
        status = honch_posix_install_signal_handler(SIGSEGV);
    }
    if (status == HONCH_OK) {
        status = honch_posix_install_signal_handler(SIGBUS);
    }
    if (status == HONCH_OK) {
        status = honch_posix_install_signal_handler(SIGILL);
    }
    if (status == HONCH_OK) {
        status = honch_posix_install_signal_handler(SIGFPE);
    }
    if (status != HONCH_OK) {
        s_error_breadcrumb_path[0] = '\0';
    }
    return status;
}

honch_status_t honch_set_property(honch_client_t *client, const char *key, honch_value_t value)
{
    return honch_core_set_property(client, key, value);
}

honch_status_t honch_session_start(honch_client_t *client, const char *session_name)
{
    return honch_core_session_start(client, session_name);
}

honch_status_t honch_session_end(honch_client_t *client)
{
    return honch_core_session_end(client);
}

honch_status_t honch_tick(honch_client_t *client)
{
    return honch_core_tick(client);
}

honch_status_t honch_flush(honch_client_t *client)
{
    return honch_core_flush(client);
}

honch_status_t honch_reset(honch_client_t *client)
{
    return honch_core_reset(client);
}

honch_status_t honch_shutdown(honch_client_t *client)
{
    honch_posix_transport_t *transport_ctx = NULL;
    if (client != NULL && client->transport != NULL) {
        transport_ctx = (honch_posix_transport_t *)client->transport->ctx;
    }

    honch_status_t status = honch_core_shutdown(client);
    if (status != HONCH_ERROR_BUSY && status != HONCH_ERROR_NOT_INITIALIZED && transport_ctx != NULL) {
        honch_posix_transport_ops_deinit(transport_ctx);
        free(transport_ctx);
    }
    return status;
}

const char *honch_get_device_id(honch_client_t *client)
{
    return honch_core_get_device_id(client);
}

honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size)
{
    return honch_core_copy_device_id(client, buffer, buffer_size);
}
