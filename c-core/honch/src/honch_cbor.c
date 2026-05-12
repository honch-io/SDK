#include "honch_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HONCH_CBOR_MAX_DEPTH 64u

typedef struct honch_json_to_cbor_parser {
    const char *cursor;
} honch_json_to_cbor_parser_t;

typedef struct honch_cbor_reader {
    const unsigned char *data;
    size_t length;
    size_t offset;
} honch_cbor_reader_t;

static honch_status_t honch_cbor_append_u8(honch_buffer_t *buffer, unsigned char value)
{
    return honch_buffer_append_n(buffer, (const char *)&value, 1u);
}

static honch_status_t honch_cbor_append_type(honch_buffer_t *buffer, unsigned char major, uint64_t value)
{
    if (value < 24u) {
        return honch_cbor_append_u8(buffer, (unsigned char)(major | value));
    }

    if (value <= UINT8_MAX) {
        unsigned char bytes[2] = { (unsigned char)(major | 24u), (unsigned char)value };
        return honch_buffer_append_n(buffer, (const char *)bytes, sizeof(bytes));
    }

    if (value <= UINT16_MAX) {
        unsigned char bytes[3] = {
            (unsigned char)(major | 25u),
            (unsigned char)(value >> 8),
            (unsigned char)value
        };
        return honch_buffer_append_n(buffer, (const char *)bytes, sizeof(bytes));
    }

    if (value <= UINT32_MAX) {
        unsigned char bytes[5] = {
            (unsigned char)(major | 26u),
            (unsigned char)(value >> 24),
            (unsigned char)(value >> 16),
            (unsigned char)(value >> 8),
            (unsigned char)value
        };
        return honch_buffer_append_n(buffer, (const char *)bytes, sizeof(bytes));
    }

    unsigned char bytes[9] = {
        (unsigned char)(major | 27u),
        (unsigned char)(value >> 56),
        (unsigned char)(value >> 48),
        (unsigned char)(value >> 40),
        (unsigned char)(value >> 32),
        (unsigned char)(value >> 24),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 8),
        (unsigned char)value
    };
    return honch_buffer_append_n(buffer, (const char *)bytes, sizeof(bytes));
}

honch_status_t honch_cbor_append_text(honch_buffer_t *buffer, const char *value)
{
    size_t length = strlen(value);
    honch_status_t status = honch_cbor_append_type(buffer, 0x60u, (uint64_t)length);
    if (status != HONCH_OK) {
        return status;
    }
    return honch_buffer_append_n(buffer, value, length);
}

honch_status_t honch_cbor_append_int(honch_buffer_t *buffer, int64_t value)
{
    if (value >= 0) {
        return honch_cbor_append_type(buffer, 0x00u, (uint64_t)value);
    }
    return honch_cbor_append_type(buffer, 0x20u, (uint64_t)(-(value + 1)));
}

honch_status_t honch_cbor_append_map(honch_buffer_t *buffer, size_t count)
{
    return honch_cbor_append_type(buffer, 0xa0u, (uint64_t)count);
}

honch_status_t honch_cbor_append_array(honch_buffer_t *buffer, size_t count)
{
    return honch_cbor_append_type(buffer, 0x80u, (uint64_t)count);
}

static honch_status_t honch_cbor_append_double(honch_buffer_t *buffer, double value)
{
    union {
        double number;
        uint64_t bits;
    } encoded;
    encoded.number = value;

    unsigned char bytes[9] = {
        0xfbu,
        (unsigned char)(encoded.bits >> 56),
        (unsigned char)(encoded.bits >> 48),
        (unsigned char)(encoded.bits >> 40),
        (unsigned char)(encoded.bits >> 32),
        (unsigned char)(encoded.bits >> 24),
        (unsigned char)(encoded.bits >> 16),
        (unsigned char)(encoded.bits >> 8),
        (unsigned char)encoded.bits
    };
    return honch_buffer_append_n(buffer, (const char *)bytes, sizeof(bytes));
}

static void honch_json_skip_ws(honch_json_to_cbor_parser_t *parser)
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

static honch_status_t honch_json_decode_string(honch_json_to_cbor_parser_t *parser, honch_buffer_t *decoded)
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
            honch_status_t status = honch_buffer_append_n(decoded, parser->cursor, 1u);
            if (status != HONCH_OK) {
                return status;
            }
            parser->cursor++;
            continue;
        }

        parser->cursor++;
        char escaped = *parser->cursor;
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                if (honch_buffer_append_n(decoded, &escaped, 1u) != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'b':
                if (honch_buffer_append(decoded, "\b") != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'f':
                if (honch_buffer_append(decoded, "\f") != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'n':
                if (honch_buffer_append(decoded, "\n") != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'r':
                if (honch_buffer_append(decoded, "\r") != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 't':
                if (honch_buffer_append(decoded, "\t") != HONCH_OK) {
                    return HONCH_ERROR_OUT_OF_MEMORY;
                }
                parser->cursor++;
                break;
            case 'u': {
                parser->cursor++;
                int codepoint = 0;
                for (size_t i = 0u; i < 4u; i++) {
                    int digit = honch_json_hex_digit_value(parser->cursor[i]);
                    if (digit < 0) {
                        return HONCH_ERROR_INVALID_ARGUMENT;
                    }
                    codepoint = (codepoint << 4) | digit;
                }
                parser->cursor += 4u;

                char decoded_char = codepoint <= 0x7f ? (char)codepoint : '?';
                if (honch_buffer_append_n(decoded, &decoded_char, 1u) != HONCH_OK) {
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

static honch_status_t honch_json_to_cbor_value(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth);

static honch_status_t honch_json_to_cbor_string(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer)
{
    honch_buffer_t decoded;
    honch_status_t status = honch_buffer_init(&decoded, 32u);
    if (status != HONCH_OK) {
        return status;
    }

    status = honch_json_decode_string(parser, &decoded);
    if (status == HONCH_OK) {
        status = honch_cbor_append_text(buffer, decoded.data);
    }
    honch_buffer_free(&decoded);
    return status;
}

static honch_status_t honch_json_to_cbor_literal(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer,
    const char *literal,
    unsigned char encoded)
{
    size_t length = strlen(literal);
    if (strncmp(parser->cursor, literal, length) != 0) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor += length;
    return honch_cbor_append_u8(buffer, encoded);
}

static honch_status_t honch_json_to_cbor_number(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer)
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

    if (buffer == NULL) {
        return HONCH_OK;
    }

    if (!floating) {
        errno = 0;
        char *end = NULL;
        long long integer = strtoll(token, &end, 10);
        if (errno == 0 && end != NULL && *end == '\0') {
            return honch_cbor_append_int(buffer, (int64_t)integer);
        }
    }

    errno = 0;
    char *end = NULL;
    double number = strtod(token, &end);
    if (errno != 0 || end == NULL || *end != '\0' || !isfinite(number)) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    return honch_cbor_append_double(buffer, number);
}

static honch_status_t honch_json_to_cbor_array(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth)
{
    if (depth > HONCH_CBOR_MAX_DEPTH || *parser->cursor != '[') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }

    parser->cursor++;
    honch_json_skip_ws(parser);
    size_t count = 0u;
    const char *items_start = parser->cursor;
    if (*parser->cursor != ']') {
        for (;;) {
            honch_status_t status = honch_json_to_cbor_value(parser, NULL, depth + 1u);
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
        status = honch_cbor_append_type(buffer, 0x80u, count);
    }
    if (status != HONCH_OK) {
        return status;
    }

    parser->cursor = items_start;
    for (size_t i = 0u; i < count; i++) {
        status = honch_json_to_cbor_value(parser, buffer, depth + 1u);
        if (status != HONCH_OK) {
            return status;
        }
        honch_json_skip_ws(parser);
        if (i + 1u < count) {
            parser->cursor++;
            honch_json_skip_ws(parser);
        }
    }

    if (*parser->cursor != ']') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor++;
    return HONCH_OK;
}

static honch_status_t honch_json_to_cbor_object(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth,
    bool filter_reserved,
    size_t *member_count)
{
    if (depth > HONCH_CBOR_MAX_DEPTH || *parser->cursor != '{') {
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
            bool include = status == HONCH_OK && (!filter_reserved || !honch_property_key_is_reserved(key.data));
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
            status = honch_json_to_cbor_value(parser, NULL, depth + 1u);
            if (status != HONCH_OK) {
                return status;
            }
            if (include) {
                count++;
            }
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
    if (buffer != NULL && !filter_reserved) {
        status = honch_cbor_append_type(buffer, 0xa0u, count);
    }
    if (status != HONCH_OK) {
        return status;
    }

    parser->cursor = members_start;
    for (size_t i = 0u, emitted = 0u; i < count || *parser->cursor != '}';) {
        if (*parser->cursor == '}') {
            break;
        }

        honch_buffer_t key;
        status = honch_buffer_init(&key, 32u);
        if (status != HONCH_OK) {
            return status;
        }
        status = honch_json_decode_string(parser, &key);
        bool include = status == HONCH_OK && (!filter_reserved || !honch_property_key_is_reserved(key.data));
        if (status == HONCH_OK && include && buffer != NULL) {
            status = honch_cbor_append_text(buffer, key.data);
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
        status = honch_json_to_cbor_value(parser, include ? buffer : NULL, depth + 1u);
        if (status != HONCH_OK) {
            return status;
        }
        if (include) {
            emitted++;
            i = emitted;
        }
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

    if (*parser->cursor != '}') {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    parser->cursor++;
    if (member_count != NULL) {
        *member_count = count;
    }
    return HONCH_OK;
}

static honch_status_t honch_json_to_cbor_value(
    honch_json_to_cbor_parser_t *parser,
    honch_buffer_t *buffer,
    size_t depth)
{
    honch_json_skip_ws(parser);
    switch (*parser->cursor) {
        case '{':
            return honch_json_to_cbor_object(parser, buffer, depth + 1u, false, NULL);
        case '[':
            return honch_json_to_cbor_array(parser, buffer, depth + 1u);
        case '"': {
            if (buffer != NULL) {
                return honch_json_to_cbor_string(parser, buffer);
            }
            honch_buffer_t decoded;
            honch_status_t status = honch_buffer_init(&decoded, 32u);
            if (status != HONCH_OK) {
                return status;
            }
            status = honch_json_decode_string(parser, &decoded);
            honch_buffer_free(&decoded);
            return status;
        }
        case 't':
            return buffer == NULL ?
                (strncmp(parser->cursor, "true", 4u) == 0 ? (parser->cursor += 4u, HONCH_OK) : HONCH_ERROR_INVALID_ARGUMENT) :
                honch_json_to_cbor_literal(parser, buffer, "true", 0xf5u);
        case 'f':
            return buffer == NULL ?
                (strncmp(parser->cursor, "false", 5u) == 0 ? (parser->cursor += 5u, HONCH_OK) : HONCH_ERROR_INVALID_ARGUMENT) :
                honch_json_to_cbor_literal(parser, buffer, "false", 0xf4u);
        case 'n':
            return buffer == NULL ?
                (strncmp(parser->cursor, "null", 4u) == 0 ? (parser->cursor += 4u, HONCH_OK) : HONCH_ERROR_INVALID_ARGUMENT) :
                honch_json_to_cbor_literal(parser, buffer, "null", 0xf6u);
        default:
            return honch_json_to_cbor_number(parser, buffer);
    }
}

honch_status_t honch_cbor_append_json_value(honch_buffer_t *buffer, const char *json)
{
    if (json == NULL) {
        return honch_cbor_append_u8(buffer, 0xf6u);
    }

    honch_json_to_cbor_parser_t parser = {.cursor = json};
    honch_status_t status = honch_json_to_cbor_value(&parser, buffer, 0u);
    if (status != HONCH_OK) {
        return status;
    }
    honch_json_skip_ws(&parser);
    return *parser.cursor == '\0' ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
}

honch_status_t honch_cbor_append_json_object_members(
    honch_buffer_t *buffer,
    const char *json,
    size_t *member_count)
{
    *member_count = 0u;
    if (!honch_json_object_has_members(json)) {
        return HONCH_OK;
    }

    honch_json_to_cbor_parser_t parser = {.cursor = json};
    honch_json_skip_ws(&parser);
    honch_status_t status = honch_json_to_cbor_object(&parser, buffer, 1u, true, member_count);
    if (status != HONCH_OK) {
        return status;
    }
    honch_json_skip_ws(&parser);
    return *parser.cursor == '\0' ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
}

static bool honch_cbor_read_uint(honch_cbor_reader_t *reader, unsigned char additional, uint64_t *out)
{
    if (additional < 24u) {
        *out = additional;
        return true;
    }

    size_t bytes = 0u;
    if (additional == 24u) {
        bytes = 1u;
    } else if (additional == 25u) {
        bytes = 2u;
    } else if (additional == 26u) {
        bytes = 4u;
    } else if (additional == 27u) {
        bytes = 8u;
    } else {
        return false;
    }

    if (reader->length - reader->offset < bytes) {
        return false;
    }
    uint64_t value = 0u;
    for (size_t i = 0u; i < bytes; i++) {
        value = (value << 8) | reader->data[reader->offset++];
    }
    *out = value;
    return true;
}

static bool honch_cbor_skip_value(honch_cbor_reader_t *reader, size_t depth)
{
    if (depth > HONCH_CBOR_MAX_DEPTH || reader->offset >= reader->length) {
        return false;
    }

    unsigned char initial = reader->data[reader->offset++];
    unsigned char major = (unsigned char)(initial & 0xe0u);
    unsigned char additional = (unsigned char)(initial & 0x1fu);
    uint64_t value = 0u;

    switch (major) {
        case 0x00u:
        case 0x20u:
            return honch_cbor_read_uint(reader, additional, &value);
        case 0x40u:
            if (!honch_cbor_read_uint(reader, additional, &value) || value > reader->length - reader->offset) {
                return false;
            }
            reader->offset += (size_t)value;
            return true;
        case 0x60u:
            if (!honch_cbor_read_uint(reader, additional, &value) || value > reader->length - reader->offset) {
                return false;
            }
            reader->offset += (size_t)value;
            return true;
        case 0x80u:
            if (!honch_cbor_read_uint(reader, additional, &value)) {
                return false;
            }
            for (uint64_t i = 0u; i < value; i++) {
                if (!honch_cbor_skip_value(reader, depth + 1u)) {
                    return false;
                }
            }
            return true;
        case 0xa0u:
            if (!honch_cbor_read_uint(reader, additional, &value)) {
                return false;
            }
            for (uint64_t i = 0u; i < value; i++) {
                if (!honch_cbor_skip_value(reader, depth + 1u) ||
                    !honch_cbor_skip_value(reader, depth + 1u)) {
                    return false;
                }
            }
            return true;
        case 0xe0u:
            if (additional == 20u || additional == 21u || additional == 22u) {
                return true;
            }
            if (additional == 25u) {
                if (reader->length - reader->offset < 2u) {
                    return false;
                }
                reader->offset += 2u;
                return true;
            }
            if (additional == 26u) {
                if (reader->length - reader->offset < 4u) {
                    return false;
                }
                reader->offset += 4u;
                return true;
            }
            if (additional == 27u) {
                if (reader->length - reader->offset < 8u) {
                    return false;
                }
                reader->offset += 8u;
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool honch_cbor_validate_event(const unsigned char *data, size_t length)
{
    if (data == NULL || length == 0u) {
        return false;
    }
    honch_cbor_reader_t reader = {
        .data = data,
        .length = length,
        .offset = 0u
    };
    return honch_cbor_skip_value(&reader, 0u) && reader.offset == length;
}
