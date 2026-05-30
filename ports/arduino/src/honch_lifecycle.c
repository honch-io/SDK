#include "honch_internal.h"

static bool honch_platform_has_lock_ops(const honch_platform_ops_t *platform)
{
    return platform != NULL &&
        platform->mutex_create != NULL &&
        platform->mutex_destroy != NULL &&
        platform->mutex_lock != NULL &&
        platform->mutex_unlock != NULL;
}

bool honch_client_lock_ops_valid(const honch_platform_ops_t *platform)
{
    if (platform == NULL) {
        return true;
    }

    bool any = platform->mutex_create != NULL ||
        platform->mutex_destroy != NULL ||
        platform->mutex_lock != NULL ||
        platform->mutex_unlock != NULL;
    return !any || honch_platform_has_lock_ops(platform);
}

honch_status_t honch_client_lock_create(honch_client_t *client, void **mutex)
{
    if (client == NULL || mutex == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    *mutex = NULL;
    if (!honch_platform_has_lock_ops(client->platform)) {
        return HONCH_OK;
    }
    return client->platform->mutex_create(client->platform->ctx, mutex);
}

void honch_client_lock_destroy(honch_client_t *client, void *mutex)
{
    if (client == NULL || !honch_platform_has_lock_ops(client->platform) || mutex == NULL) {
        return;
    }
    client->platform->mutex_destroy(client->platform->ctx, mutex);
}

static honch_status_t honch_client_lock_handle(honch_client_t *client, void *mutex)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    if (!honch_platform_has_lock_ops(client->platform)) {
        return HONCH_OK;
    }
    return client->platform->mutex_lock(client->platform->ctx, mutex);
}

static void honch_client_unlock_handle(honch_client_t *client, void *mutex)
{
    if (client == NULL || !honch_platform_has_lock_ops(client->platform)) {
        return;
    }
    client->platform->mutex_unlock(client->platform->ctx, mutex);
}

honch_status_t honch_client_enter(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    honch_status_t status = honch_client_lock_handle(client, client->lifetime_mutex);
    if (status != HONCH_OK) {
        return status;
    }

    if (client->closing) {
        honch_client_unlock_handle(client, client->lifetime_mutex);
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    client->active_calls++;
    honch_client_unlock_handle(client, client->lifetime_mutex);
    return HONCH_OK;
}

void honch_client_leave(honch_client_t *client)
{
    if (client == NULL || honch_client_lock_handle(client, client->lifetime_mutex) != HONCH_OK) {
        return;
    }

    if (client->active_calls > 0u) {
        client->active_calls--;
    }

    honch_client_unlock_handle(client, client->lifetime_mutex);
}

honch_status_t honch_client_begin_shutdown(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    honch_status_t status = honch_client_lock_handle(client, client->lifetime_mutex);
    if (status != HONCH_OK) {
        return status;
    }

    if (client->closing) {
        honch_client_unlock_handle(client, client->lifetime_mutex);
        return HONCH_ERROR_NOT_INITIALIZED;
    }
    if (client->active_calls > 0u) {
        honch_client_unlock_handle(client, client->lifetime_mutex);
        return HONCH_ERROR_BUSY;
    }

    client->closing = true;
    honch_client_unlock_handle(client, client->lifetime_mutex);
    return HONCH_OK;
}

honch_status_t honch_client_state_lock(honch_client_t *client)
{
    return honch_client_lock_handle(client, client->state_mutex);
}

void honch_client_state_unlock(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }
    honch_client_unlock_handle(client, client->state_mutex);
}
