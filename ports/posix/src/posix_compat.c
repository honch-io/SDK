#include "honch/honch.h"
#include "honch/posix/honch.h"

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
        .flush_event_threshold = config->flush_event_threshold,
        .flush_retry_initial_ms = config->flush_retry_initial_ms,
        .flush_retry_max_ms = config->flush_retry_max_ms,
        .battery_callback = config->battery_callback,
        .battery_low_threshold = config->battery_low_threshold,
        .auto_properties_callback = config->auto_properties_callback,
        .auto_properties_userdata = config->auto_properties_userdata,
        .durability_mode = config->durability_mode
    };
}

honch_status_t honch_init(honch_client_t **client, const honch_config_t *config)
{
    if (config == NULL) {
        return honch_core_init(client, NULL);
    }

    honch_platform_ops_t platform_ops;
    honch_storage_ops_t storage_ops;
    honch_transport_ops_t transport_ops;
    honch_posix_platform_t platform_ctx;
    honch_posix_storage_t storage_ctx;
    honch_posix_transport_t transport_ctx;

    honch_status_t status = honch_posix_platform_ops_init(&platform_ops, &platform_ctx);
    if (status == HONCH_OK) {
        status = honch_posix_storage_ops_init(&storage_ops, &storage_ctx, config->queue_directory);
    }
    if (status == HONCH_OK) {
        status = honch_posix_transport_ops_init(&transport_ops, &transport_ctx);
    }
    if (status != HONCH_OK) {
        return status;
    }

    honch_core_config_t core_config;
    honch_posix_config_to_core(config, &core_config);
    core_config.platform = &platform_ops;
    core_config.storage = &storage_ops;
    core_config.transport = &transport_ops;
    return honch_core_init(client, &core_config);
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
    return honch_core_shutdown(client);
}

const char *honch_get_device_id(honch_client_t *client)
{
    return honch_core_get_device_id(client);
}

honch_status_t honch_copy_device_id(honch_client_t *client, char *buffer, size_t buffer_size)
{
    return honch_core_copy_device_id(client, buffer, buffer_size);
}
