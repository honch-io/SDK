#include "honch_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char HONCH_BOOT_PROPERTIES[] = "{\"reset_reason\":\"unknown\"}";

static honch_status_t honch_validate_event_name(const char *event_name)
{
    if (honch_is_blank(event_name) || strlen(event_name) > HONCH_MAX_EVENT_NAME) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_validate_distinct_id(const char *distinct_id)
{
    if (honch_is_blank(distinct_id) || strlen(distinct_id) > HONCH_MAX_DISTINCT_ID) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

static honch_status_t honch_validate_property_key(const char *key)
{
    if (honch_is_blank(key) || strlen(key) > HONCH_MAX_PROPERTY_KEY) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    for (const unsigned char *cursor = (const unsigned char *)key; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
    }
    return HONCH_OK;
}

static honch_status_t honch_append_property_pair(
    honch_buffer_t *buffer,
    bool *has_members,
    const char *key,
    const char *value)
{
    honch_status_t status = HONCH_OK;
    if (*has_members) {
        status = honch_buffer_append(buffer, ",");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(buffer, key);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(buffer, ":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(buffer, value);
    }
    if (status == HONCH_OK) {
        *has_members = true;
    }
    return status;
}

static honch_status_t honch_append_raw_property_pair(
    honch_buffer_t *buffer,
    bool *has_members,
    const char *key,
    const char *value_json)
{
    honch_status_t status = HONCH_OK;
    if (*has_members) {
        status = honch_buffer_append(buffer, ",");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(buffer, key);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(buffer, ":");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(buffer, value_json);
    }
    if (status == HONCH_OK) {
        *has_members = true;
    }
    return status;
}

static honch_status_t honch_append_properties_object(
    honch_client_t *client,
    honch_buffer_t *buffer,
    const char *properties_json,
    int battery_level)
{
    bool has_members = false;
    honch_status_t status = honch_buffer_append(buffer, "\"properties\":{");

    if (status == HONCH_OK && honch_json_object_has_members(properties_json)) {
        status = honch_json_append_object_members(buffer, properties_json);
        has_members = true;
    }
    if (status == HONCH_OK) {
        status = honch_state_append_properties(client, buffer, &has_members);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$device_id", client->device_id);
    }
    if (status == HONCH_OK && client->session_id != NULL) {
        status = honch_append_property_pair(buffer, &has_members, "$session_id", client->session_id);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$device_model", client->device_model);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$firmware_version", client->firmware_version);
    }
    if (status == HONCH_OK && client->environment != NULL) {
        status = honch_append_property_pair(buffer, &has_members, "$environment", client->environment);
    }
    if (status == HONCH_OK && battery_level >= 0 && battery_level <= 100) {
        char level_json[4];
        snprintf(level_json, sizeof(level_json), "%d", battery_level);
        status = honch_append_raw_property_pair(buffer, &has_members, "$battery_level", level_json);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$sdk_name", HONCH_SDK_NAME);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$sdk_version", HONCH_SDK_VERSION);
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(buffer, &has_members, "$sdk_platform", "c-posix");
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(buffer, "}");
    }

    return status;
}

static honch_status_t honch_build_event(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    int battery_level,
    char **out)
{
    char uuid[33];
    honch_status_t status = honch_random_hex(uuid);
    if (status != HONCH_OK) {
        return status;
    }
    char timestamp[25];
    status = honch_now_iso8601(timestamp);
    if (status != HONCH_OK) {
        return status;
    }

    honch_buffer_t buffer;
    status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&buffer, "{\"uuid\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&buffer, uuid);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, ",\"timestamp\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&buffer, timestamp);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, ",\"distinct_id\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&buffer, client->distinct_id);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, ",\"event\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&buffer, event_name);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, ",");
    }
    if (status == HONCH_OK) {
        status = honch_append_properties_object(client, &buffer, properties_json, battery_level);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, "}");
    }

    if (status == HONCH_OK && buffer.length > client->max_event_bytes) {
        status = HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    *out = buffer.data;
    return HONCH_OK;
}

static int honch_read_battery_level(honch_client_t *client)
{
    if (client->battery_callback == NULL) {
        return -1;
    }

    return client->battery_callback();
}

static honch_status_t honch_track_locked_internal(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    bool check_battery_low);
static void honch_scheduler_notify_after_enqueue_locked(honch_client_t *client);

static honch_status_t honch_emit_battery_low_locked(honch_client_t *client, int battery_level)
{
    if (battery_level < 0) {
        return HONCH_OK;
    }

    if (battery_level >= client->battery_low_threshold) {
        client->battery_low_emitted = false;
        return HONCH_OK;
    }

    if (client->battery_low_emitted) {
        return HONCH_OK;
    }

    client->battery_low_emitted = true;
    char properties_json[32];
    snprintf(properties_json, sizeof(properties_json), "{\"level\":%d}", battery_level);
    return honch_track_locked_internal(client, "$battery_low", properties_json, false);
}

static honch_status_t honch_track_locked_internal(
    honch_client_t *client,
    const char *event_name,
    const char *properties_json,
    bool check_battery_low)
{
    int battery_level = honch_read_battery_level(client);
    char *event = NULL;
    honch_status_t status = honch_build_event(client, event_name, properties_json, battery_level, &event);
    if (status == HONCH_OK) {
        status = honch_queue_enqueue(client, event);
    }
    if (status == HONCH_OK) {
        honch_scheduler_notify_after_enqueue_locked(client);
    }
    if (status == HONCH_OK && check_battery_low) {
        status = honch_emit_battery_low_locked(client, battery_level);
    }

    free(event);
    return status;
}

static honch_status_t honch_track_locked(honch_client_t *client, const char *event_name, const char *properties_json)
{
    return honch_track_locked_internal(client, event_name, properties_json, true);
}

static uint64_t honch_scheduler_interval_ms(honch_client_t *client)
{
    return (uint64_t)client->flush_interval_seconds * 1000u;
}

static unsigned int honch_next_retry_delay_ms(honch_client_t *client)
{
    unsigned int delay = client->current_retry_delay_ms;
    if (delay == 0u) {
        delay = client->flush_retry_initial_ms;
    }

    unsigned int quarter = delay / 4u;
    if (quarter == 0u) {
        return delay;
    }

    uint64_t now = honch_now_millis();
    unsigned int jitter = (unsigned int)(now % ((uint64_t)(quarter * 2u) + 1u));
    return (delay - quarter) + jitter;
}

static void honch_grow_retry_delay(honch_client_t *client)
{
    unsigned int next = client->current_retry_delay_ms == 0u ?
        client->flush_retry_initial_ms :
        client->current_retry_delay_ms * 2u;
    if (next < client->current_retry_delay_ms || next > client->flush_retry_max_ms) {
        next = client->flush_retry_max_ms;
    }
    client->current_retry_delay_ms = next;
}

static bool honch_status_is_retryable(honch_status_t status)
{
    return status == HONCH_ERROR_TRANSPORT ||
           status == HONCH_ERROR_RATE_LIMITED ||
           status == HONCH_ERROR_SERVER;
}

static void honch_scheduler_make_deadline(struct timespec *deadline, uint64_t wait_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(wait_ms / 1000u);
    deadline->tv_nsec += (long)((wait_ms % 1000u) * 1000000u);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static uint64_t honch_scheduler_wait_ms(honch_client_t *client, uint64_t now)
{
    if (client->next_retry_flush_ms > now) {
        return client->next_retry_flush_ms - now;
    }

    uint64_t wait_ms = UINT64_MAX;
    if (client->scheduler_flush_requested) {
        wait_ms = 0u;
    }
    if (client->flush_interval_seconds > 0u) {
        uint64_t interval_wait = client->next_interval_flush_ms > now ?
            client->next_interval_flush_ms - now :
            0u;
        if (interval_wait < wait_ms) {
            wait_ms = interval_wait;
        }
    }
    return wait_ms;
}

static void honch_scheduler_notify_after_enqueue_locked(honch_client_t *client)
{
    if (!client->scheduler_enabled || client->flush_event_threshold == 0u) {
        return;
    }

    size_t pending_count = 0u;
    if (honch_queue_count_pending(client, &pending_count) == HONCH_OK &&
        pending_count >= client->flush_event_threshold) {
        client->scheduler_flush_requested = true;
        if (client->scheduler_started) {
            pthread_cond_signal(&client->scheduler_cond);
        }
    }
}

static void *honch_scheduler_main(void *userdata)
{
    honch_client_t *client = (honch_client_t *)userdata;
    pthread_mutex_lock(&client->mutex);

    while (!client->scheduler_stop) {
        uint64_t now = honch_now_millis();
        bool retry_blocked = client->next_retry_flush_ms > now;
        bool interval_due = client->flush_interval_seconds > 0u && now >= client->next_interval_flush_ms;
        bool should_flush = (client->scheduler_flush_requested || interval_due) && !retry_blocked;

        if (should_flush) {
            client->scheduler_flush_requested = false;
            honch_status_t status = honch_queue_flush_locked(client);
            now = honch_now_millis();

            if (status == HONCH_OK) {
                client->current_retry_delay_ms = client->flush_retry_initial_ms;
                client->next_retry_flush_ms = 0u;
            } else if (honch_status_is_retryable(status)) {
                uint64_t wait_ms = honch_next_retry_delay_ms(client);
                client->next_retry_flush_ms = now + wait_ms;
                client->scheduler_flush_requested = true;
                honch_grow_retry_delay(client);
            }

            if (client->flush_interval_seconds > 0u) {
                client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
            }
            continue;
        }

        uint64_t wait_ms = honch_scheduler_wait_ms(client, now);
        if (wait_ms == UINT64_MAX) {
            pthread_cond_wait(&client->scheduler_cond, &client->mutex);
        } else {
            struct timespec deadline;
            honch_scheduler_make_deadline(&deadline, wait_ms);
            pthread_cond_timedwait(&client->scheduler_cond, &client->mutex, &deadline);
        }
    }

    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

static honch_status_t honch_scheduler_start(honch_client_t *client)
{
    if (!client->scheduler_enabled) {
        return HONCH_OK;
    }

    uint64_t now = honch_now_millis();
    client->current_retry_delay_ms = client->flush_retry_initial_ms;
    if (client->flush_interval_seconds > 0u) {
        client->next_interval_flush_ms = now + honch_scheduler_interval_ms(client);
    }

    if (client->flush_event_threshold > 0u) {
        size_t pending_count = 0u;
        honch_status_t status = honch_queue_count_pending(client, &pending_count);
        if (status != HONCH_OK) {
            return status;
        }
        client->scheduler_flush_requested = pending_count >= client->flush_event_threshold;
    }

    if (pthread_create(&client->scheduler_thread, NULL, honch_scheduler_main, client) != 0) {
        return HONCH_ERROR_IO;
    }
    client->scheduler_started = true;
    return HONCH_OK;
}

static void honch_scheduler_stop(honch_client_t *client)
{
    if (!client->scheduler_started) {
        return;
    }

    pthread_mutex_lock(&client->mutex);
    client->scheduler_stop = true;
    pthread_cond_signal(&client->scheduler_cond);
    pthread_mutex_unlock(&client->mutex);

    pthread_join(client->scheduler_thread, NULL);
    client->scheduler_started = false;
}

static honch_status_t honch_new_session_id(char **out)
{
    char random[33];
    honch_status_t status = honch_random_hex(random);
    if (status != HONCH_OK) {
        return status;
    }

    char *session_id = (char *)malloc(38u);
    if (session_id == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    snprintf(session_id, 38u, "sess_%s", random);
    *out = session_id;
    return HONCH_OK;
}

static honch_status_t honch_build_session_start_properties(const char *session_name, char **out)
{
    *out = NULL;
    if (honch_is_blank(session_name)) {
        return HONCH_OK;
    }

    honch_buffer_t properties;
    honch_status_t status = honch_buffer_init(&properties, strlen(session_name) + 32u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&properties, "{\"session_name\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, session_name);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&properties);
        return status;
    }

    *out = properties.data;
    return HONCH_OK;
}

static honch_status_t honch_build_firmware_update_properties(
    const char *previous_version,
    const char *new_version,
    char **out)
{
    honch_buffer_t properties;
    honch_status_t status = honch_buffer_init(
        &properties,
        strlen(previous_version) + strlen(new_version) + 64u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&properties, "{\"previous_version\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, previous_version);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, ",\"new_version\":");
    }
    if (status == HONCH_OK) {
        status = honch_json_append_string(&properties, new_version);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&properties);
        return status;
    }

    *out = properties.data;
    return HONCH_OK;
}

static honch_status_t honch_emit_firmware_update_locked(honch_client_t *client)
{
    bool changed = false;
    char *previous_version = NULL;
    honch_status_t status = honch_state_check_firmware_version(client, &changed, &previous_version);
    if (status != HONCH_OK || !changed) {
        free(previous_version);
        return status;
    }

    char *properties_json = NULL;
    status = honch_build_firmware_update_properties(previous_version, client->firmware_version, &properties_json);
    if (status == HONCH_OK) {
        status = honch_track_locked(client, "$firmware_update", properties_json);
    }

    free(properties_json);
    free(previous_version);
    return status;
}

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config)
{
    if (client == NULL || config == NULL || honch_is_blank(config->api_key) ||
        honch_is_blank(config->endpoint_url) || honch_is_blank(config->device_model) ||
        honch_is_blank(config->firmware_version) || honch_is_blank(config->queue_directory)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    *client = NULL;

    honch_client_t *next = (honch_client_t *)calloc(1u, sizeof(*next));
    if (next == NULL) {
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    next->api_key = honch_strdup(config->api_key);
    next->endpoint_url = honch_strdup(config->endpoint_url);
    next->queue_directory = honch_strdup(config->queue_directory);
    next->batch_size = config->batch_size == 0u ? HONCH_DEFAULT_BATCH_SIZE : config->batch_size;
    next->max_queued_events = config->max_queued_events == 0u ? HONCH_DEFAULT_MAX_QUEUED_EVENTS : config->max_queued_events;
    next->max_event_bytes = config->max_event_bytes == 0u ? HONCH_DEFAULT_MAX_EVENT_BYTES : config->max_event_bytes;
    next->transport_timeout_ms = config->transport_timeout_ms == 0u ?
        HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS :
        config->transport_timeout_ms;
    next->flush_interval_seconds = config->flush_interval_seconds;
    next->flush_event_threshold = config->flush_event_threshold;
    next->flush_retry_initial_ms = config->flush_retry_initial_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS :
        config->flush_retry_initial_ms;
    next->flush_retry_max_ms = config->flush_retry_max_ms == 0u ?
        HONCH_DEFAULT_FLUSH_RETRY_MAX_MS :
        config->flush_retry_max_ms;
    if (next->flush_retry_max_ms < next->flush_retry_initial_ms) {
        next->flush_retry_max_ms = next->flush_retry_initial_ms;
    }
    next->scheduler_enabled = next->flush_interval_seconds > 0u || next->flush_event_threshold > 0u;
    next->battery_callback = config->battery_callback;
    next->battery_low_threshold = config->battery_low_threshold > 0 ?
        config->battery_low_threshold :
        HONCH_DEFAULT_BATTERY_LOW_THRESHOLD;

    honch_status_t status = HONCH_OK;
    bool mutex_initialized = false;
    bool scheduler_cond_initialized = false;
    if (next->api_key == NULL || next->endpoint_url == NULL || next->queue_directory == NULL) {
        status = HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (status == HONCH_OK) {
        status = honch_state_prepare(next, config);
    }
    if (status == HONCH_OK && pthread_mutex_init(&next->mutex, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        mutex_initialized = true;
    }
    if (status == HONCH_OK && pthread_cond_init(&next->scheduler_cond, NULL) != 0) {
        status = HONCH_ERROR_IO;
    } else if (status == HONCH_OK) {
        scheduler_cond_initialized = true;
    }
    if (status == HONCH_OK) {
        status = honch_emit_firmware_update_locked(next);
    }
    if (status == HONCH_OK) {
        status = honch_track_locked(next, "$device_boot", HONCH_BOOT_PROPERTIES);
    }
    if (status == HONCH_OK) {
        status = honch_scheduler_start(next);
    }

    if (status != HONCH_OK) {
        honch_scheduler_stop(next);
        if (scheduler_cond_initialized) {
            pthread_cond_destroy(&next->scheduler_cond);
        }
        if (mutex_initialized) {
            pthread_mutex_destroy(&next->mutex);
        }
        honch_free_client_fields(next);
        free(next);
        return status;
    }

    *client = next;
    return HONCH_OK;
}

honch_status_t honch_track(honch_client_t *client, const char *event_name, const char *properties_json)
{
    if (client == NULL || honch_validate_event_name(event_name) != HONCH_OK ||
        !honch_json_is_object(properties_json)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);

    honch_status_t status = honch_track_locked(client, event_name, properties_json);
    pthread_mutex_unlock(&client->mutex);
    return status;
}

honch_status_t honch_identify(honch_client_t *client, const char *distinct_id, const char *traits_json)
{
    if (client == NULL || honch_validate_distinct_id(distinct_id) != HONCH_OK ||
        !honch_json_is_object(traits_json)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);

    char *previous_distinct_id = honch_strdup(client->distinct_id);
    char *next_distinct_id = honch_strdup(distinct_id);
    if (previous_distinct_id == NULL || next_distinct_id == NULL) {
        free(previous_distinct_id);
        free(next_distinct_id);
        pthread_mutex_unlock(&client->mutex);
        return HONCH_ERROR_OUT_OF_MEMORY;
    }

    free(client->distinct_id);
    client->distinct_id = next_distinct_id;
    next_distinct_id = NULL;

    honch_buffer_t properties;
    honch_status_t status = honch_buffer_init(&properties, 256u);
    bool has_members = false;
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "{");
    }
    if (status == HONCH_OK) {
        status = honch_append_property_pair(&properties, &has_members, "$anon_distinct_id", previous_distinct_id);
    }
    if (status == HONCH_OK) {
        status = honch_append_raw_property_pair(&properties, &has_members, "$set", traits_json == NULL ? "{}" : traits_json);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&properties, "}");
    }

    if (status == HONCH_OK) {
        status = honch_track_locked(client, "$identify", properties.data);
    }
    if (status == HONCH_OK) {
        status = honch_state_save_distinct_id(client);
    }

    if (status != HONCH_OK) {
        free(client->distinct_id);
        client->distinct_id = previous_distinct_id;
        previous_distinct_id = NULL;
    }

    free(previous_distinct_id);
    honch_buffer_free(&properties);
    pthread_mutex_unlock(&client->mutex);
    return status;
}

honch_status_t honch_set_property(honch_client_t *client, const char *key, const char *value_json)
{
    if (client == NULL || honch_validate_property_key(key) != HONCH_OK ||
        value_json == NULL || !honch_json_is_value(value_json)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);
    honch_status_t status = honch_state_set_property(client, key, value_json);
    pthread_mutex_unlock(&client->mutex);
    return status;
}

honch_status_t honch_session_start(honch_client_t *client, const char *session_name)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    char *session_id = NULL;
    honch_status_t status = honch_new_session_id(&session_id);
    if (status != HONCH_OK) {
        return status;
    }

    char *properties_json = NULL;
    status = honch_build_session_start_properties(session_name, &properties_json);
    if (status != HONCH_OK) {
        free(session_id);
        return status;
    }

    pthread_mutex_lock(&client->mutex);

    if (client->session_id != NULL) {
        status = honch_track_locked(client, "$session_end", NULL);
        if (status == HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }
    if (status == HONCH_OK) {
        client->session_id = session_id;
        session_id = NULL;
        status = honch_track_locked(client, "$session_start", properties_json);
        if (status != HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }

    pthread_mutex_unlock(&client->mutex);
    free(properties_json);
    free(session_id);
    return status;
}

honch_status_t honch_session_end(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);

    honch_status_t status = HONCH_OK;
    if (client->session_id != NULL) {
        status = honch_track_locked(client, "$session_end", NULL);
        if (status == HONCH_OK) {
            free(client->session_id);
            client->session_id = NULL;
        }
    }

    pthread_mutex_unlock(&client->mutex);
    return status;
}

honch_status_t honch_flush(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);
    honch_status_t status = honch_queue_flush_locked(client);
    pthread_mutex_unlock(&client->mutex);
    return status;
}

honch_status_t honch_reset(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);
    honch_status_t status = honch_queue_clear(client);
    if (status == HONCH_OK) {
        status = honch_track_locked(client, "$device_reset", NULL);
    }
    if (status == HONCH_OK) {
        status = honch_state_reset(client);
    }
    if (status == HONCH_OK) {
        free(client->session_id);
        client->session_id = NULL;
        client->battery_low_emitted = false;
    }
    pthread_mutex_unlock(&client->mutex);
    return status;
}

void honch_shutdown(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

    bool flush_on_shutdown = client->scheduler_enabled;
    honch_scheduler_stop(client);

    pthread_mutex_lock(&client->mutex);
    (void)honch_track_locked(client, "$device_shutdown", NULL);
    if (flush_on_shutdown) {
        (void)honch_queue_flush_locked(client);
    }
    pthread_mutex_unlock(&client->mutex);

    pthread_cond_destroy(&client->scheduler_cond);
    pthread_mutex_destroy(&client->mutex);
    honch_free_client_fields(client);
    free(client);
}

const char *honch_get_device_id(honch_client_t *client)
{
    if (client == NULL) {
        return NULL;
    }

    return client->device_id;
}

const char *honch_status_string(honch_status_t status)
{
    switch (status) {
        case HONCH_OK:
            return "ok";
        case HONCH_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case HONCH_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case HONCH_ERROR_IO:
            return "io error";
        case HONCH_ERROR_TRANSPORT:
            return "transport error";
        case HONCH_ERROR_RATE_LIMITED:
            return "rate limited";
        case HONCH_ERROR_SERVER:
            return "server error";
        case HONCH_ERROR_REJECTED:
            return "rejected";
        default:
            return "unknown";
    }
}
