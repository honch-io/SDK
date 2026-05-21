#include "honch_internal.h"

honch_status_t honch_client_enter(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    if (pthread_mutex_lock(&client->lifetime_mutex) != 0) {
        return HONCH_ERROR_IO;
    }

    if (client->closing) {
        (void)pthread_mutex_unlock(&client->lifetime_mutex);
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    client->active_calls++;
    (void)pthread_mutex_unlock(&client->lifetime_mutex);
    return HONCH_OK;
}

void honch_client_leave(honch_client_t *client)
{
    if (client == NULL) {
        return;
    }

    if (pthread_mutex_lock(&client->lifetime_mutex) != 0) {
        return;
    }

    if (client->active_calls > 0u) {
        client->active_calls--;
        if (client->closing && client->active_calls == 0u) {
            pthread_cond_signal(&client->lifetime_cond);
        }
    }

    (void)pthread_mutex_unlock(&client->lifetime_mutex);
}

honch_status_t honch_client_begin_shutdown(honch_client_t *client)
{
    if (client == NULL) {
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    if (pthread_mutex_lock(&client->lifetime_mutex) != 0) {
        return HONCH_ERROR_IO;
    }

    if (client->closing) {
        (void)pthread_mutex_unlock(&client->lifetime_mutex);
        return HONCH_ERROR_NOT_INITIALIZED;
    }

    client->closing = true;
    while (client->active_calls > 0u) {
        pthread_cond_wait(&client->lifetime_cond, &client->lifetime_mutex);
    }

    (void)pthread_mutex_unlock(&client->lifetime_mutex);
    return HONCH_OK;
}
