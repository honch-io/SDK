#include "honch_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *properties_json)
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
    if (status == HONCH_OK && client->device_model != NULL) {
        status = honch_append_property_pair(buffer, &has_members, "$device_model", client->device_model);
    }
    if (status == HONCH_OK && client->firmware_version != NULL) {
        status = honch_append_property_pair(buffer, &has_members, "$firmware_version", client->firmware_version);
    }
    if (status == HONCH_OK && client->environment != NULL) {
        status = honch_append_property_pair(buffer, &has_members, "$environment", client->environment);
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
    char **out)
{
    char uuid[33];
    honch_status_t status = honch_random_hex(uuid);
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
        status = honch_buffer_appendf(&buffer, ",\"timestamp\":%llu", (unsigned long long)honch_now_millis());
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
        status = honch_append_properties_object(client, &buffer, properties_json);
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

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config)
{
    if (client == NULL || config == NULL || honch_is_blank(config->api_key) ||
        honch_is_blank(config->endpoint_url) || honch_is_blank(config->queue_directory)) {
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

    honch_status_t status = HONCH_OK;
    if (next->api_key == NULL || next->endpoint_url == NULL || next->queue_directory == NULL) {
        status = HONCH_ERROR_OUT_OF_MEMORY;
    }
    if (status == HONCH_OK) {
        status = honch_state_prepare(next, config);
    }
    if (status == HONCH_OK && pthread_mutex_init(&next->mutex, NULL) != 0) {
        status = HONCH_ERROR_IO;
    }

    if (status != HONCH_OK) {
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

    char *event = NULL;
    honch_status_t status = honch_build_event(client, event_name, properties_json, &event);
    if (status == HONCH_OK) {
        status = honch_queue_enqueue(client, event);
    }

    free(event);
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

    char *event = NULL;
    if (status == HONCH_OK) {
        status = honch_build_event(client, "$identify", properties.data, &event);
    }
    if (status == HONCH_OK) {
        status = honch_queue_enqueue(client, event);
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
    free(event);
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
    honch_status_t status = honch_state_reset(client);
    pthread_mutex_unlock(&client->mutex);
    return status;
}

void honch_shutdown(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

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
