#include "honch_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct honch_json_parser {
    const char *cursor;
} honch_json_parser_t;

#define HONCH_JSON_MAX_DEPTH 64u

static void honch_json_skip_ws(honch_json_parser_t *parser)
{
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\n' || *parser->cursor == '\r') {
        parser->cursor++;
    }
}

static bool honch_json_parse_value(honch_json_parser_t *parser, size_t depth);

static bool honch_json_parse_hex4(honch_json_parser_t *parser)
{
    for (size_t i = 0u; i < 4u; i++) {
        if (!isxdigit((unsigned char)*parser->cursor)) {
            return false;
        }
        parser->cursor++;
    }
    return true;
}

static bool honch_json_parse_string(honch_json_parser_t *parser)
{
    if (*parser->cursor != '"') {
        return false;
    }
    parser->cursor++;

    while (*parser->cursor != '\0') {
        unsigned char ch = (unsigned char)*parser->cursor;
        if (ch == '"') {
            parser->cursor++;
            return true;
        }
        if (ch < 0x20u) {
            return false;
        }
        if (ch == '\\') {
            parser->cursor++;
            switch (*parser->cursor) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    parser->cursor++;
                    break;
                case 'u':
                    parser->cursor++;
                    if (!honch_json_parse_hex4(parser)) {
                        return false;
                    }
                    break;
                default:
                    return false;
            }
        } else {
            parser->cursor++;
        }
    }

    return false;
}

static bool honch_json_parse_literal(honch_json_parser_t *parser, const char *literal)
{
    size_t length = strlen(literal);
    if (strncmp(parser->cursor, literal, length) != 0) {
        return false;
    }
    parser->cursor += length;
    return true;
}

static bool honch_json_parse_number(honch_json_parser_t *parser)
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
        return false;
    }

    if (*parser->cursor == '.') {
        parser->cursor++;
        if (!isdigit((unsigned char)*parser->cursor)) {
            return false;
        }
        while (isdigit((unsigned char)*parser->cursor)) {
            parser->cursor++;
        }
    }

    if (*parser->cursor == 'e' || *parser->cursor == 'E') {
        parser->cursor++;
        if (*parser->cursor == '+' || *parser->cursor == '-') {
            parser->cursor++;
        }
        if (!isdigit((unsigned char)*parser->cursor)) {
            return false;
        }
        while (isdigit((unsigned char)*parser->cursor)) {
            parser->cursor++;
        }
    }

    return parser->cursor > start;
}

static bool honch_json_parse_array(honch_json_parser_t *parser, size_t depth)
{
    if (depth > HONCH_JSON_MAX_DEPTH) {
        return false;
    }

    if (*parser->cursor != '[') {
        return false;
    }
    parser->cursor++;
    honch_json_skip_ws(parser);
    if (*parser->cursor == ']') {
        parser->cursor++;
        return true;
    }

    for (;;) {
        honch_json_skip_ws(parser);
        if (!honch_json_parse_value(parser, depth)) {
            return false;
        }
        honch_json_skip_ws(parser);
        if (*parser->cursor == ']') {
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') {
            return false;
        }
        parser->cursor++;
    }
}

static bool honch_json_parse_object(honch_json_parser_t *parser, size_t depth)
{
    if (depth > HONCH_JSON_MAX_DEPTH) {
        return false;
    }

    if (*parser->cursor != '{') {
        return false;
    }
    parser->cursor++;
    honch_json_skip_ws(parser);
    if (*parser->cursor == '}') {
        parser->cursor++;
        return true;
    }

    for (;;) {
        honch_json_skip_ws(parser);
        if (!honch_json_parse_string(parser)) {
            return false;
        }
        honch_json_skip_ws(parser);
        if (*parser->cursor != ':') {
            return false;
        }
        parser->cursor++;
        honch_json_skip_ws(parser);
        if (!honch_json_parse_value(parser, depth)) {
            return false;
        }
        honch_json_skip_ws(parser);
        if (*parser->cursor == '}') {
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') {
            return false;
        }
        parser->cursor++;
    }
}

static bool honch_json_parse_value(honch_json_parser_t *parser, size_t depth)
{
    honch_json_skip_ws(parser);
    switch (*parser->cursor) {
        case '{':
            return honch_json_parse_object(parser, depth + 1u);
        case '[':
            return honch_json_parse_array(parser, depth + 1u);
        case '"':
            return honch_json_parse_string(parser);
        case 't':
            return honch_json_parse_literal(parser, "true");
        case 'f':
            return honch_json_parse_literal(parser, "false");
        case 'n':
            return honch_json_parse_literal(parser, "null");
        default:
            return honch_json_parse_number(parser);
    }
}

bool honch_json_is_value(const char *json)
{
    if (json == NULL) {
        return true;
    }

    honch_json_parser_t parser = {.cursor = json};
    honch_json_skip_ws(&parser);
    if (!honch_json_parse_value(&parser, 0u)) {
        return false;
    }
    honch_json_skip_ws(&parser);
    return *parser.cursor == '\0';
}

bool honch_json_is_object(const char *json)
{
    if (json == NULL) {
        return true;
    }

    honch_json_parser_t parser = {.cursor = json};
    honch_json_skip_ws(&parser);
    if (!honch_json_parse_object(&parser, 1u)) {
        return false;
    }
    honch_json_skip_ws(&parser);
    return *parser.cursor == '\0';
}

honch_status_t honch_json_append_string(honch_buffer_t *buffer, const char *value)
{
    honch_status_t status = honch_buffer_append(buffer, "\"");
    if (status != HONCH_OK) {
        return status;
    }

    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            case '"':
                status = honch_buffer_append(buffer, "\\\"");
                break;
            case '\\':
                status = honch_buffer_append(buffer, "\\\\");
                break;
            case '\b':
                status = honch_buffer_append(buffer, "\\b");
                break;
            case '\f':
                status = honch_buffer_append(buffer, "\\f");
                break;
            case '\n':
                status = honch_buffer_append(buffer, "\\n");
                break;
            case '\r':
                status = honch_buffer_append(buffer, "\\r");
                break;
            case '\t':
                status = honch_buffer_append(buffer, "\\t");
                break;
            default:
                if (*cursor < 0x20u) {
                    status = honch_buffer_appendf(buffer, "\\u%04x", *cursor);
                } else {
                    status = honch_buffer_append_n(buffer, (const char *)cursor, 1u);
                }
                break;
        }

        if (status != HONCH_OK) {
            return status;
        }
    }

    return honch_buffer_append(buffer, "\"");
}
