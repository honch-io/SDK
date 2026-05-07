#include "honch_internal.h"

#include <stdlib.h>

honch_status_t honch_encoder_build_batch_json(
    honch_client_t *client,
    const honch_file_list_t *files,
    size_t count,
    char **out)
{
    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_buffer_append(&buffer, "{\"token\":");
    if (status == HONCH_OK) {
        status = honch_json_append_string(&buffer, client->api_key);
    }
    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, ",\"batch\":[");
    }

    for (size_t i = 0u; status == HONCH_OK && i < count; i++) {
        char *event_json = NULL;
        status = honch_read_file(files->items[i].path, &event_json);
        if (status != HONCH_OK) {
            break;
        }

        if (i > 0u) {
            status = honch_buffer_append(&buffer, ",");
        }
        if (status == HONCH_OK) {
            status = honch_buffer_append(&buffer, event_json);
        }
        free(event_json);
    }

    if (status == HONCH_OK) {
        status = honch_buffer_append(&buffer, "]}");
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    *out = buffer.data;
    return HONCH_OK;
}
