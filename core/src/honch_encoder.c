#include "honch_internal.h"

#include <stdlib.h>

honch_status_t honch_encoder_build_batch_cbor(
    honch_client_t *client,
    const honch_file_list_t *files,
    size_t count,
    honch_payload_t *out,
    size_t *invalid_index)
{
    out->data = NULL;
    out->length = 0u;
    *invalid_index = count;

    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 1024u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_cbor_append_map(&buffer, 2u);
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "token");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, client->api_key);
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(&buffer, "batch");
    }
    if (status == HONCH_OK) {
        status = honch_cbor_append_array(&buffer, count);
    }

    for (size_t i = 0u; status == HONCH_OK && i < count; i++) {
        honch_payload_t event = {0};
        status = honch_read_file_limited_bytes(files->items[i].path, client->max_event_bytes, &event);
        if (status != HONCH_OK) {
            if (status == HONCH_ERROR_INVALID_ARGUMENT) {
                *invalid_index = i;
            }
            break;
        }
        if (!honch_cbor_validate_event(event.data, event.length)) {
            free(event.data);
            *invalid_index = i;
            status = HONCH_ERROR_INVALID_ARGUMENT;
            break;
        }

        status = honch_buffer_append_n(&buffer, (const char *)event.data, event.length);
        free(event.data);
    }

    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }

    out->data = (unsigned char *)buffer.data;
    out->length = buffer.length;
    return HONCH_OK;
}
