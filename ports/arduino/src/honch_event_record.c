#include "honch_internal.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_EVENT_RECORD_MAX_DEPTH 64u
#define HONCH_EVENT_RECORD_MAX_PROPERTIES 64u

enum {
    HONCH_RECORD_NULL = 0u,
    HONCH_RECORD_FALSE = 1u,
    HONCH_RECORD_TRUE = 2u,
    HONCH_RECORD_UINT = 3u,
    HONCH_RECORD_INT = 4u,
    HONCH_RECORD_FLOAT32 = 5u,
    HONCH_RECORD_FLOAT64 = 6u,
    HONCH_RECORD_STRING = 7u,
    HONCH_RECORD_BYTES = 8u,
    HONCH_RECORD_ARRAY = 9u,
    HONCH_RECORD_MAP = 10u
};

typedef struct honch_record_json_parser {
    const char *cursor;
} honch_record_json_parser_t;

typedef struct honch_record_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
} honch_record_reader_t;

static const uint8_t HONCH_EVENT_RECORD_MAGIC[] = {'H', 'Q', 'R', '1'};

static void honch_json_skip_ws(honch_record_json_parser_t *parser)
{
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\n' || *parser->cursor == '\r') {
        parser->cursor++;
    }
}

static int honch_json_hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static honch_status_t honch_record_append_byte(honch_buffer_t *buffer, uint8_t value)
{
    return honch_buffer_append_n(buffer, (const char *)&value, 1u);
}

static honch_status_t honch_record_append_uvarint(honch_buffer_t *buffer, uint64_t value)
{
    uint8_t encoded[10];
    size_t used = 0u;
    honch_status_t status = honch_wire_v2_encode_uvarint(value, encoded, sizeof(encoded), &used);
    if (status != HONCH_OK) {
        return status;
    }
    return honch_buffer_append_n(buffer, (const char *)encoded, used);
}

static honch_status_t honch_record_append_string_n(honch_buffer_t *buffer, const char *value, size_t length, bool allow_empty)
{
    if (value == NULL || (!allow_empty && length == 0u) || !honch_utf8_is_valid(value, length)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_record_append_uvarint(buffer, (uint64_t)length);
    if (status == HONCH_OK) {
        status = honch_buffer_append_n(buffer, value, length);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_byte(buffer, 0u);
    }
    return status;
}

static honch_status_t honch_record_append_string(honch_buffer_t *buffer, const char *value, bool allow_empty)
{
    return honch_record_append_string_n(buffer, value, strlen(value), allow_empty);
}

static honch_status_t honch_json_buffer_append(void *ctx, const char *value, size_t length)
{
    return honch_buffer_append_n((honch_buffer_t *)ctx, value, length);
}

static honch_status_t honch_json_decode_string_to(
    honch_record_json_parser_t *parser,
    honch_status_t (*append)(void *ctx, const char *value, size_t length),
    void *append_ctx)
{
    if (*parser->cursor != '"') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor++;

    while (*parser->cursor != '\0') {
        unsigned char ch = (unsigned char)*parser->cursor;
        if (ch == '"') {
            parser->cursor++;
            return HONCH_OK;
        }
        if (ch < 0x20u) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (ch != '\\') {
            const char *start = parser->cursor;
            do {
                parser->cursor++;
                ch = (unsigned char)*parser->cursor;
            } while (ch != '\0' && ch != '"' && ch != '\\' && ch >= 0x20u);
            size_t length = (size_t)(parser->cursor - start);
            if (!honch_utf8_is_valid(start, length)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_status_t status = append(append_ctx, start, length);
            if (status != HONCH_OK) {
                return status;
            }
            continue;
        }

        parser->cursor++;
        char escaped = *parser->cursor;
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                if (append(append_ctx, &escaped, 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'b':
                if (append(append_ctx, "\b", 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'f':
                if (append(append_ctx, "\f", 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'n':
                if (append(append_ctx, "\n", 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'r':
                if (append(append_ctx, "\r", 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 't':
                if (append(append_ctx, "\t", 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'u': {
                parser->cursor++;
                unsigned int codepoint = 0u;
                for (size_t i = 0u; i < 4u; i++) {
                    int digit = honch_json_hex_digit_value(parser->cursor[i]);
                    if (digit < 0) {
                        return HONCH_ERROR_INVALID_ARGUMENT;
                    }
                    codepoint = (codepoint << 4) | (unsigned int)digit;
                }
                parser->cursor += 4u;
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                    if (parser->cursor[0] != '\\' || parser->cursor[1] != 'u') {
                        return HONCH_ERROR_INVALID_ARGUMENT;
                    }
                    parser->cursor += 2u;
                    unsigned int low = 0u;
                    for (size_t i = 0u; i < 4u; i++) {
                        int digit = honch_json_hex_digit_value(parser->cursor[i]);
                        if (digit < 0) {
                            return HONCH_ERROR_INVALID_ARGUMENT;
                        }
                        low = (low << 4) | (unsigned int)digit;
                    }
                    if (low < 0xdc00u || low > 0xdfffu) {
                        return HONCH_ERROR_INVALID_ARGUMENT;
                    }
                    parser->cursor += 4u;
                    codepoint = 0x10000u + (((codepoint - 0xd800u) << 10) | (low - 0xdc00u));
                } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                    return HONCH_ERROR_INVALID_ARGUMENT;
                }
                char encoded[4];
                size_t encoded_length = 0u;
                if (codepoint <= 0x7fu) {
                    encoded[0] = (char)codepoint;
                    encoded_length = 1u;
                } else if (codepoint <= 0x7ffu) {
                    encoded[0] = (char)(0xc0u | (codepoint >> 6));
                    encoded[1] = (char)(0x80u | (codepoint & 0x3fu));
                    encoded_length = 2u;
                } else if (codepoint <= 0xffffu) {
                    encoded[0] = (char)(0xe0u | (codepoint >> 12));
                    encoded[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
                    encoded[2] = (char)(0x80u | (codepoint & 0x3fu));
                    encoded_length = 3u;
                } else if (codepoint <= 0x10ffffu) {
                    encoded[0] = (char)(0xf0u | (codepoint >> 18));
                    encoded[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
                    encoded[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
                    encoded[3] = (char)(0x80u | (codepoint & 0x3fu));
                    encoded_length = 4u;
                } else {
                    return HONCH_ERROR_INVALID_ARGUMENT;
                }
                if (append(append_ctx, encoded, encoded_length) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                break;
            }
            default:
                return HONCH_ERROR_INVALID_ARGUMENT;
        }
    }
    return HONCH_ERROR_INVALID_ARGUMENT;
}

static honch_status_t honch_json_decode_string(honch_record_json_parser_t *parser, honch_buffer_t *decoded)
{
    return honch_json_decode_string_to(parser, honch_json_buffer_append, decoded);
}

static honch_status_t honch_record_append_json_value(
    honch_record_json_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth);

static honch_status_t honch_record_append_json_string_value(honch_record_json_parser_t *parser, honch_buffer_t *buffer)
{
    honch_buffer_t decoded;
    honch_status_t status = honch_buffer_init(&decoded, 32u);
    if (status != HONCH_OK) {
        return status;
    }
    status = honch_json_decode_string(parser, &decoded);
    if (status == HONCH_OK) {
        status = honch_record_append_byte(buffer, HONCH_RECORD_STRING);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_string_n(buffer, decoded.data, decoded.length, true);
    }
    honch_buffer_free(&decoded);
    return status;
}

static honch_status_t honch_record_append_json_literal(
    honch_record_json_parser_t *parser,
    honch_buffer_t *buffer,
    const char *literal,
    uint8_t tag)
{
    size_t length = strlen(literal);
    if (strncmp(parser->cursor, literal, length) != 0) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor += length;
    return honch_record_append_byte(buffer, tag);
}

static honch_status_t honch_record_append_json_number(honch_record_json_parser_t *parser, honch_buffer_t *buffer)
{
    const char *start = parser->cursor;
    if (*parser->cursor == '-') {
        parser->cursor++;
    }
    if (*parser->cursor == '0') {
        parser->cursor++;
    } else if (isdigit((unsigned char)*parser->cursor)) {
        while (isdigit((unsigned char)*parser->cursor)) {
            parser->cursor++;
        }
    } else {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    bool floating = false;
    if (*parser->cursor == '.') {
        floating = true;
        parser->cursor++;
        if (!isdigit((unsigned char)*parser->cursor)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        while (isdigit((unsigned char)*parser->cursor)) {
            parser->cursor++;
        }
    }
    if (*parser->cursor == 'e' || *parser->cursor == 'E') {
        floating = true;
        parser->cursor++;
        if (*parser->cursor == '+' || *parser->cursor == '-') {
            parser->cursor++;
        }
        if (!isdigit((unsigned char)*parser->cursor)) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        while (isdigit((unsigned char)*parser->cursor)) {
            parser->cursor++;
        }
    }

    size_t length = (size_t)(parser->cursor - start);
    char token[128];
    if (length >= sizeof(token)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memcpy(token, start, length);
    token[length] = '\0';

    if (!floating) {
        errno = 0;
        char *end = NULL;
        long long integer = strtoll(token, &end, 10);
        if (errno != 0 || end == NULL || *end != '\0') {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        honch_status_t status = HONCH_OK;
        if (integer >= 0) {
            status = honch_record_append_byte(buffer, HONCH_RECORD_UINT);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, (uint64_t)integer);
            }
        } else {
            status = honch_record_append_byte(buffer, HONCH_RECORD_INT);
            if (status == HONCH_OK) {
                status = honch_record_append_uvarint(buffer, honch_wire_v2_zigzag_i64((int64_t)integer));
            }
        }
        return status;
    }

    errno = 0;
    char *end = NULL;
    double number = strtod(token, &end);
    if (errno != 0 || end == NULL || *end != '\0' || !isfinite(number)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    union {
        double number;
        uint64_t bits;
    } encoded = {.number = number};
    honch_status_t status = honch_record_append_byte(buffer, HONCH_RECORD_FLOAT64);
    for (size_t i = 0u; status == HONCH_OK && i < sizeof(encoded.bits); i++) {
        status = honch_record_append_byte(buffer, (uint8_t)(encoded.bits >> (i * 8u)));
    }
    return status;
}

static honch_status_t honch_record_append_json_array(
    honch_record_json_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth)
{
    if (depth > HONCH_EVENT_RECORD_MAX_DEPTH || *parser->cursor != '[') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor++;
    honch_json_skip_ws(parser);
    size_t count = 0u;
    const char *items_start = parser->cursor;
    if (*parser->cursor != ']') {
        for (;;) {
            honch_status_t status = honch_record_append_json_value(parser, NULL, depth + 1u);
            if (status != HONCH_OK) {
                return status;
            }
            count++;
            honch_json_skip_ws(parser);
            if (*parser->cursor == ']') {
                break;
            }
            if (*parser->cursor != ',') {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            parser->cursor++;
            honch_json_skip_ws(parser);
        }
    }
    honch_status_t status = HONCH_OK;
    if (buffer != NULL) {
        status = honch_record_append_byte(buffer, HONCH_RECORD_ARRAY);
        if (status == HONCH_OK) {
            status = honch_record_append_uvarint(buffer, (uint64_t)count);
        }
    }
    parser->cursor = items_start;
    for (size_t i = 0u; status == HONCH_OK && i < count; i++) {
        status = honch_record_append_json_value(parser, buffer, depth + 1u);
        honch_json_skip_ws(parser);
        if (i + 1u < count) {
            parser->cursor++;
            honch_json_skip_ws(parser);
        }
    }
    if (status != HONCH_OK || *parser->cursor != ']') {
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }
    parser->cursor++;
    return HONCH_OK;
}

static honch_status_t honch_record_append_json_object(
    honch_record_json_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth)
{
    if (depth > HONCH_EVENT_RECORD_MAX_DEPTH || *parser->cursor != '{') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor++;
    honch_json_skip_ws(parser);
    size_t count = 0u;
    const char *members_start = parser->cursor;
    if (*parser->cursor != '}') {
        for (;;) {
            honch_buffer_t key;
            honch_status_t status = honch_buffer_init(&key, 32u);
            if (status != HONCH_OK) {
                return status;
            }
            status = honch_json_decode_string(parser, &key);
            honch_buffer_free(&key);
            if (status != HONCH_OK) {
                return status;
            }
            honch_json_skip_ws(parser);
            if (*parser->cursor != ':') {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            parser->cursor++;
            honch_json_skip_ws(parser);
            status = honch_record_append_json_value(parser, NULL, depth + 1u);
            if (status != HONCH_OK) {
                return status;
            }
            count++;
            honch_json_skip_ws(parser);
            if (*parser->cursor == '}') {
                break;
            }
            if (*parser->cursor != ',') {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            parser->cursor++;
            honch_json_skip_ws(parser);
        }
    }
    honch_status_t status = HONCH_OK;
    if (buffer != NULL) {
        status = honch_record_append_byte(buffer, HONCH_RECORD_MAP);
        if (status == HONCH_OK) {
            status = honch_record_append_uvarint(buffer, (uint64_t)count);
        }
    }
    parser->cursor = members_start;
    for (size_t i = 0u; status == HONCH_OK && i < count; i++) {
        honch_buffer_t key;
        status = honch_buffer_init(&key, 32u);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_json_decode_string(parser, &key);
        if (status == HONCH_OK && memchr(key.data, '\0', key.length) != NULL) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (status == HONCH_OK && buffer != NULL) {
            status = honch_record_append_string_n(buffer, key.data, key.length, false);
        }
        honch_buffer_free(&key);
        if (status != HONCH_OK) {
            return status;
        }
        honch_json_skip_ws(parser);
        if (*parser->cursor != ':') {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        parser->cursor++;
        honch_json_skip_ws(parser);
        status = honch_record_append_json_value(parser, buffer, depth + 1u);
        honch_json_skip_ws(parser);
        if (i + 1u < count) {
            parser->cursor++;
            honch_json_skip_ws(parser);
        }
    }
    if (status != HONCH_OK || *parser->cursor != '}') {
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }
    parser->cursor++;
    return HONCH_OK;
}

static honch_status_t honch_record_append_json_value(
    honch_record_json_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth)
{
    honch_json_skip_ws(parser);
    if (buffer == NULL) {
        honch_buffer_t scratch;
        honch_status_t status = honch_buffer_init(&scratch, 32u);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_record_append_json_value(parser, &scratch, depth);
        honch_buffer_free(&scratch);
        return status;
    }
    switch (*parser->cursor) {
        case '{':
            return honch_record_append_json_object(parser, buffer, depth + 1u);
        case '[':
            return honch_record_append_json_array(parser, buffer, depth + 1u);
        case '"':
            return honch_record_append_json_string_value(parser, buffer);
        case 't':
            return honch_record_append_json_literal(parser, buffer, "true", HONCH_RECORD_TRUE);
        case 'f':
            return honch_record_append_json_literal(parser, buffer, "false", HONCH_RECORD_FALSE);
        case 'n':
            return honch_record_append_json_literal(parser, buffer, "null", HONCH_RECORD_NULL);
        default:
            return honch_record_append_json_number(parser, buffer);
    }
}

honch_status_t honch_event_record_append_json_value(honch_buffer_t *buffer, const char *json)
{
    if (json == NULL) {
        return honch_record_append_byte(buffer, HONCH_RECORD_NULL);
    }
    honch_record_json_parser_t parser = {.cursor = json};
    honch_status_t status = honch_record_append_json_value(&parser, buffer, 0u);
    if (status != HONCH_OK) {
        return status;
    }
    honch_json_skip_ws(&parser);
    return *parser.cursor == '\0' ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
}

honch_status_t honch_event_record_append_property_json(
    honch_buffer_t *buffer,
    size_t *member_count,
    const char *key,
    const char *value_json)
{
    if (buffer == NULL || member_count == NULL || honch_is_blank(key) || memchr(key, '\0', strlen(key)) != NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    honch_status_t status = honch_record_append_string(buffer, key, false);
    if (status == HONCH_OK) {
        status = honch_event_record_append_json_value(buffer, value_json);
    }
    if (status == HONCH_OK) {
        (*member_count)++;
    }
    return status;
}

honch_status_t honch_event_record_append_json_object_members(
    honch_buffer_t *buffer,
    const char *json,
    size_t *member_count)
{
    *member_count = 0u;
    if (json == NULL) {
        return HONCH_OK;
    }
    honch_record_json_parser_t parser = {.cursor = json};
    honch_json_skip_ws(&parser);
    if (*parser.cursor != '{') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser.cursor++;
    honch_json_skip_ws(&parser);
    if (*parser.cursor == '}') {
        parser.cursor++;
        honch_json_skip_ws(&parser);
        return *parser.cursor == '\0' ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
    }
    for (;;) {
        honch_buffer_t key;
        honch_status_t status = honch_buffer_init(&key, 32u);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_json_decode_string(&parser, &key);
        if (status == HONCH_OK && memchr(key.data, '\0', key.length) != NULL) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        }
        bool include = status == HONCH_OK && !honch_property_key_is_reserved(key.data);
        if (status == HONCH_OK && include && *member_count >= HONCH_EVENT_RECORD_MAX_PROPERTIES) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (status == HONCH_OK && include) {
            status = honch_record_append_string_n(buffer, key.data, key.length, false);
        }
        honch_buffer_free(&key);
        if (status != HONCH_OK) {
            return status;
        }
        honch_json_skip_ws(&parser);
        if (*parser.cursor != ':') {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        parser.cursor++;
        honch_json_skip_ws(&parser);
        status = honch_record_append_json_value(&parser, include ? buffer : NULL, 1u);
        if (status != HONCH_OK) {
            return status;
        }
        if (include) {
            (*member_count)++;
        }
        honch_json_skip_ws(&parser);
        if (*parser.cursor == '}') {
            parser.cursor++;
            honch_json_skip_ws(&parser);
            return *parser.cursor == '\0' ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
        }
        if (*parser.cursor != ',') {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        parser.cursor++;
        honch_json_skip_ws(&parser);
    }
}

honch_status_t honch_event_record_build(
    const char *event_name,
    const char *distinct_id,
    const char *session_id,
    uint64_t timestamp_ms,
    const honch_buffer_t *properties,
    size_t property_count,
    honch_payload_t *out)
{
    if (out == NULL || honch_is_blank(event_name) || honch_is_blank(distinct_id) ||
        properties == NULL || property_count > HONCH_EVENT_RECORD_MAX_PROPERTIES) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    out->data = NULL;
    out->length = 0u;
    honch_buffer_t buffer;
    honch_status_t status = honch_buffer_init(&buffer, 128u + properties->length);
    if (status != HONCH_OK) {
        return status;
    }
    status = honch_buffer_append_n(&buffer, (const char *)HONCH_EVENT_RECORD_MAGIC, sizeof(HONCH_EVENT_RECORD_MAGIC));
    if (status == HONCH_OK) {
        status = honch_record_append_uvarint(&buffer, timestamp_ms);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_string(&buffer, distinct_id, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_byte(&buffer, session_id != NULL ? 1u : 0u);
    }
    if (status == HONCH_OK && session_id != NULL) {
        status = honch_record_append_string(&buffer, session_id, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_string(&buffer, event_name, false);
    }
    if (status == HONCH_OK) {
        status = honch_record_append_uvarint(&buffer, (uint64_t)property_count);
    }
    if (status == HONCH_OK && properties->length > 0u) {
        status = honch_buffer_append_n(&buffer, properties->data, properties->length);
    }
    if (status != HONCH_OK) {
        honch_buffer_free(&buffer);
        return status;
    }
    out->data = (unsigned char *)buffer.data;
    out->length = buffer.length;
    return HONCH_OK;
}

static honch_status_t honch_record_read_uvarint(honch_record_reader_t *reader, uint64_t *out)
{
    uint64_t value = 0u;
    unsigned int shift = 0u;
    for (size_t i = 0u; i < 10u; i++) {
        if (reader->offset >= reader->length) {
            return HONCH_ERROR_INVALID_ARGUMENT;
        }
        uint8_t byte = reader->data[reader->offset++];
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *out = value;
            return HONCH_OK;
        }
        shift += 7u;
    }
    return HONCH_ERROR_INVALID_ARGUMENT;
}

static honch_status_t honch_record_read_string(honch_record_reader_t *reader, const char **out, size_t *out_size, bool allow_empty)
{
    uint64_t length = 0u;
    honch_status_t status = honch_record_read_uvarint(reader, &length);
    if (status != HONCH_OK || length > SIZE_MAX || (size_t)length + 1u > reader->length - reader->offset) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    const char *value = (const char *)reader->data + reader->offset;
    if ((!allow_empty && length == 0u) || value[length] != '\0' || !honch_utf8_is_valid(value, (size_t)length)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    reader->offset += (size_t)length + 1u;
    *out = value;
    if (out_size != NULL) {
        *out_size = (size_t)length;
    }
    return HONCH_OK;
}

static void honch_record_value_free(honch_wire_v2_value_t *value)
{
    if (value == NULL) {
        return;
    }
    if (value->type == HONCH_WIRE_V2_VALUE_TYPE_ARRAY) {
        honch_wire_v2_value_t *items = (honch_wire_v2_value_t *)value->array.items;
        for (size_t i = 0u; i < value->array.count; i++) {
            honch_record_value_free(&items[i]);
        }
        free(items);
    } else if (value->type == HONCH_WIRE_V2_VALUE_TYPE_MAP) {
        honch_wire_v2_map_pair_t *entries = (honch_wire_v2_map_pair_t *)value->map.entries;
        for (size_t i = 0u; i < value->map.count; i++) {
            honch_record_value_free(&entries[i].value);
        }
        free(entries);
    }
    memset(value, 0, sizeof(*value));
}

static honch_status_t honch_record_read_value(honch_record_reader_t *reader, honch_wire_v2_value_t *value, size_t depth)
{
    if (reader->offset >= reader->length || depth > HONCH_EVENT_RECORD_MAX_DEPTH) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    uint8_t tag = reader->data[reader->offset++];
    switch (tag) {
        case HONCH_RECORD_NULL:
            *value = (honch_wire_v2_value_t){.type = HONCH_WIRE_V2_VALUE_TYPE_NULL};
            return HONCH_OK;
        case HONCH_RECORD_FALSE:
        case HONCH_RECORD_TRUE:
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_BOOL,
                .bool_value = tag == HONCH_RECORD_TRUE
            };
            return HONCH_OK;
        case HONCH_RECORD_UINT: {
            uint64_t number = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &number);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){.type = HONCH_WIRE_V2_VALUE_TYPE_UINT, .uint_value = number};
            return HONCH_OK;
        }
        case HONCH_RECORD_INT: {
            uint64_t number = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &number);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_INT,
                .int_value = honch_wire_v2_unzigzag_i64(number)
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_FLOAT32: {
            if (reader->length - reader->offset < 4u) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint32_t bits = 0u;
            for (size_t i = 0u; i < 4u; i++) {
                bits |= (uint32_t)reader->data[reader->offset++] << (i * 8u);
            }
            union {
                uint32_t bits;
                float number;
            } decoded = {.bits = bits};
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT32,
                .float32_value = decoded.number
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_FLOAT64: {
            if (reader->length - reader->offset < 8u) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            uint64_t bits = 0u;
            for (size_t i = 0u; i < 8u; i++) {
                bits |= (uint64_t)reader->data[reader->offset++] << (i * 8u);
            }
            union {
                uint64_t bits;
                double number;
            } decoded = {.bits = bits};
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64,
                .float64_value = decoded.number
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_STRING: {
            const char *string = NULL;
            size_t string_size = 0u;
            honch_status_t status = honch_record_read_string(reader, &string, &string_size, true);
            if (status != HONCH_OK) {
                return status;
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = string,
                .string_size = string_size
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_ARRAY: {
            uint64_t count = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &count);
            if (status != HONCH_OK || count > SIZE_MAX / sizeof(honch_wire_v2_value_t)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_wire_v2_value_t *items = NULL;
            if (count > 0u) {
                items = (honch_wire_v2_value_t *)calloc((size_t)count, sizeof(*items));
                if (items == NULL) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
            }
            for (uint64_t i = 0u; i < count; i++) {
                status = honch_record_read_value(reader, &items[i], depth + 1u);
                if (status != HONCH_OK) {
                    for (uint64_t j = 0u; j < i; j++) {
                        honch_record_value_free(&items[j]);
                    }
                    free(items);
                    return status;
                }
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
                .array = {.items = items, .count = (size_t)count}
            };
            return HONCH_OK;
        }
        case HONCH_RECORD_MAP: {
            uint64_t count = 0u;
            honch_status_t status = honch_record_read_uvarint(reader, &count);
            if (status != HONCH_OK || count > SIZE_MAX / sizeof(honch_wire_v2_map_pair_t)) {
                return HONCH_ERROR_INVALID_ARGUMENT;
            }
            honch_wire_v2_map_pair_t *entries = NULL;
            if (count > 0u) {
                entries = (honch_wire_v2_map_pair_t *)calloc((size_t)count, sizeof(*entries));
                if (entries == NULL) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
            }
            for (uint64_t i = 0u; i < count; i++) {
                status = honch_record_read_string(reader, &entries[i].key, NULL, false);
                if (status == HONCH_OK) {
                    status = honch_record_read_value(reader, &entries[i].value, depth + 1u);
                }
                if (status != HONCH_OK) {
                    for (uint64_t j = 0u; j < i; j++) {
                        honch_record_value_free(&entries[j].value);
                    }
                    free(entries);
                    return status;
                }
            }
            *value = (honch_wire_v2_value_t){
                .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
                .map = {.entries = entries, .count = (size_t)count}
            };
            return HONCH_OK;
        }
        default:
            return HONCH_ERROR_INVALID_ARGUMENT;
    }
}

honch_status_t honch_event_record_parse(const uint8_t *data, size_t length, honch_event_record_t *record)
{
    if (data == NULL || record == NULL || length < sizeof(HONCH_EVENT_RECORD_MAGIC) ||
        memcmp(data, HONCH_EVENT_RECORD_MAGIC, sizeof(HONCH_EVENT_RECORD_MAGIC)) != 0) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    memset(record, 0, sizeof(*record));
    honch_record_reader_t reader = {
        .data = data,
        .length = length,
        .offset = sizeof(HONCH_EVENT_RECORD_MAGIC)
    };
    honch_status_t status = honch_record_read_uvarint(&reader, &record->timestamp_ms);
    if (status == HONCH_OK) {
        status = honch_record_read_string(&reader, &record->distinct_id, NULL, false);
    }
    if (status == HONCH_OK) {
        if (reader.offset >= reader.length || reader.data[reader.offset] > 1u) {
            status = HONCH_ERROR_INVALID_ARGUMENT;
        } else if (reader.data[reader.offset++] == 1u) {
            status = honch_record_read_string(&reader, &record->session_id, NULL, false);
        }
    }
    if (status == HONCH_OK) {
        status = honch_record_read_string(&reader, &record->event_name, NULL, false);
    }
    uint64_t property_count = 0u;
    if (status == HONCH_OK) {
        status = honch_record_read_uvarint(&reader, &property_count);
    }
    if (status != HONCH_OK || property_count > HONCH_EVENT_RECORD_MAX_PROPERTIES) {
        return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
    }
    for (uint64_t i = 0u; i < property_count; i++) {
        status = honch_record_read_string(&reader, &record->properties[i].key, NULL, false);
        if (status == HONCH_OK) {
            status = honch_record_read_value(&reader, &record->properties[i].value, 0u);
        }
        if (status != HONCH_OK) {
            honch_event_record_free(record);
            return status;
        }
        record->property_count++;
    }
    if (reader.offset != reader.length || record->timestamp_ms == 0u) {
        honch_event_record_free(record);
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return HONCH_OK;
}

void honch_event_record_free(honch_event_record_t *record)
{
    if (record == NULL) {
        return;
    }
    for (size_t i = 0u; i < record->property_count; i++) {
        honch_record_value_free(&record->properties[i].value);
    }
    memset(record, 0, sizeof(*record));
}

static bool honch_record_property_is_promoted_context(const char *key)
{
    static const char *promoted[] = {
        "$device_id",
        "$device_model",
        "$firmware_version",
        "$sdk_platform",
        "$sdk_version",
        "$environment",
        "$session_id"
    };
    for (size_t i = 0u; i < sizeof(promoted) / sizeof(promoted[0]); i++) {
        if (strcmp(key, promoted[i]) == 0) {
            return true;
        }
    }
    return false;
}

void honch_event_record_prepare_wire_properties(honch_event_record_t *record)
{
    if (record == NULL) {
        return;
    }
    size_t write_index = 0u;
    for (size_t read_index = 0u; read_index < record->property_count; read_index++) {
        honch_wire_v2_property_t property = record->properties[read_index];
        if (honch_record_property_is_promoted_context(property.key)) {
            honch_record_value_free(&property.value);
            continue;
        }
        if (write_index != read_index) {
            record->properties[write_index] = property;
            memset(&record->properties[read_index], 0, sizeof(record->properties[read_index]));
        }
        write_index++;
    }
    record->property_count = write_index;
}

bool honch_event_record_validate(const uint8_t *data, size_t length)
{
    honch_event_record_t record;
    honch_status_t status = honch_event_record_parse(data, length, &record);
    honch_event_record_free(&record);
    return status == HONCH_OK;
}
