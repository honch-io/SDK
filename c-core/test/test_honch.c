#include "honch/honch.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define EXPECT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)
#define EXPECT_EQ_INT(a, b) do { int av = (int)(a); int bv = (int)(b); if (av != bv) { fprintf(stderr, "FAIL %s:%d: %s (%d != %d)\n", __FILE__, __LINE__, #a " == " #b, av, bv); failures++; } } while (0)
#define EXPECT_STR_CONTAINS(haystack, needle) EXPECT_TRUE(strstr((haystack), (needle)) != NULL)
#define EXPECT_STR_NOT_CONTAINS(haystack, needle) EXPECT_TRUE(strstr((haystack), (needle)) == NULL)

static int extract_timestamp_millis(const char *json, long long *out)
{
    const char *marker = "\"timestamp\":";
    const char *start = strstr(json, marker);
    if (start == NULL) {
        return 0;
    }
    start += strlen(marker);
    char *end = NULL;
    long long value = strtoll(start, &end, 10);
    if (end == start) {
        return 0;
    }
    *out = value;
    return 1;
}

static void make_temp_dir(char *path, size_t size)
{
    snprintf(path, size, "/tmp/honch-test-%ld-XXXXXX", (long)getpid());
    char *result = mkdtemp(path);
    EXPECT_TRUE(result != NULL);
}

static size_t count_files_with_suffix(const char *directory, const char *suffix)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return 0u;
    }

    size_t suffix_length = strlen(suffix);
    size_t count = 0u;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length >= suffix_length && strcmp(entry->d_name + length - suffix_length, suffix) == 0) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

static int read_text_file(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    size_t read_count = fread(out, 1u, size - 1u, file);
    fclose(file);
    out[read_count] = '\0';
    while (read_count > 0u &&
           (out[read_count - 1u] == '\n' || out[read_count - 1u] == '\r' ||
            out[read_count - 1u] == ' ' || out[read_count - 1u] == '\t')) {
        out[read_count - 1u] = '\0';
        read_count--;
    }
    return 1;
}

static int write_text_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    size_t length = strlen(content);
    int ok = fwrite(content, 1u, length, file) == length;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static int write_bytes_file(const char *path, const unsigned char *content, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    int ok = fwrite(content, 1u, length, file) == length;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static int count_substring(const char *haystack, const char *needle)
{
    int count = 0;
    const char *cursor = haystack;
    size_t needle_length = strlen(needle);
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_length;
    }
    return count;
}

typedef struct cbor_reader {
    const unsigned char *data;
    size_t length;
    size_t offset;
} cbor_reader_t;

static int append_text(char *out, size_t out_size, size_t *used, const char *text, size_t length)
{
    if (*used >= out_size) {
        return 0;
    }
    for (size_t i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        const char *escaped = NULL;
        char unicode[8];
        switch (ch) {
            case '"':
                escaped = "\\\"";
                break;
            case '\\':
                escaped = "\\\\";
                break;
            case '\b':
                escaped = "\\b";
                break;
            case '\f':
                escaped = "\\f";
                break;
            case '\n':
                escaped = "\\n";
                break;
            case '\r':
                escaped = "\\r";
                break;
            case '\t':
                escaped = "\\t";
                break;
            default:
                if (ch < 0x20u) {
                    snprintf(unicode, sizeof(unicode), "\\u%04x", ch);
                    escaped = unicode;
                }
                break;
        }
        if (escaped != NULL) {
            size_t escaped_length = strlen(escaped);
            if (*used + escaped_length >= out_size) {
                return 0;
            }
            memcpy(out + *used, escaped, escaped_length);
            *used += escaped_length;
        } else {
            if (*used + 1u >= out_size) {
                return 0;
            }
            out[(*used)++] = (char)ch;
        }
    }
    out[*used] = '\0';
    return 1;
}

static int append_literal(char *out, size_t out_size, size_t *used, const char *literal)
{
    size_t length = strlen(literal);
    if (*used + length >= out_size) {
        return 0;
    }
    memcpy(out + *used, literal, length);
    *used += length;
    out[*used] = '\0';
    return 1;
}

static int cbor_read_uint(cbor_reader_t *reader, unsigned char additional, unsigned long long *out)
{
    if (additional < 24u) {
        *out = additional;
        return 1;
    }
    size_t bytes = additional == 24u ? 1u : additional == 25u ? 2u : additional == 26u ? 4u : additional == 27u ? 8u : 0u;
    if (bytes == 0u || reader->length - reader->offset < bytes) {
        return 0;
    }
    unsigned long long value = 0u;
    for (size_t i = 0u; i < bytes; i++) {
        value = (value << 8) | reader->data[reader->offset++];
    }
    *out = value;
    return 1;
}

static int cbor_to_json_value(cbor_reader_t *reader, char *out, size_t out_size, size_t *used);

static int cbor_to_json_items(cbor_reader_t *reader, unsigned long long count, char close, char *out, size_t out_size, size_t *used)
{
    for (unsigned long long i = 0u; i < count; i++) {
        if (i > 0u && !append_literal(out, out_size, used, ",")) {
            return 0;
        }
        if (!cbor_to_json_value(reader, out, out_size, used)) {
            return 0;
        }
    }
    char close_text[2] = { close, '\0' };
    return append_literal(out, out_size, used, close_text);
}

static int cbor_to_json_value(cbor_reader_t *reader, char *out, size_t out_size, size_t *used)
{
    if (reader->offset >= reader->length) {
        return 0;
    }
    unsigned char initial = reader->data[reader->offset++];
    unsigned char major = (unsigned char)(initial & 0xe0u);
    unsigned char additional = (unsigned char)(initial & 0x1fu);
    unsigned long long value = 0u;

    switch (major) {
        case 0x00u:
            if (!cbor_read_uint(reader, additional, &value)) {
                return 0;
            }
            char positive[32];
            snprintf(positive, sizeof(positive), "%llu", value);
            return append_literal(out, out_size, used, positive);
        case 0x20u:
            if (!cbor_read_uint(reader, additional, &value)) {
                return 0;
            }
            char negative[32];
            snprintf(negative, sizeof(negative), "-%llu", value + 1u);
            return append_literal(out, out_size, used, negative);
        case 0x60u:
            if (!cbor_read_uint(reader, additional, &value) || value > reader->length - reader->offset) {
                return 0;
            }
            if (!append_literal(out, out_size, used, "\"") ||
                !append_text(out, out_size, used, (const char *)reader->data + reader->offset, (size_t)value) ||
                !append_literal(out, out_size, used, "\"")) {
                return 0;
            }
            reader->offset += (size_t)value;
            return 1;
        case 0x80u:
            if (!cbor_read_uint(reader, additional, &value) || !append_literal(out, out_size, used, "[")) {
                return 0;
            }
            return cbor_to_json_items(reader, value, ']', out, out_size, used);
        case 0xa0u:
            if (!cbor_read_uint(reader, additional, &value) || !append_literal(out, out_size, used, "{")) {
                return 0;
            }
            for (unsigned long long i = 0u; i < value; i++) {
                if (i > 0u && !append_literal(out, out_size, used, ",")) {
                    return 0;
                }
                if (!cbor_to_json_value(reader, out, out_size, used) ||
                    !append_literal(out, out_size, used, ":") ||
                    !cbor_to_json_value(reader, out, out_size, used)) {
                    return 0;
                }
            }
            return append_literal(out, out_size, used, "}");
        case 0xe0u:
            if (additional == 20u) {
                return append_literal(out, out_size, used, "false");
            }
            if (additional == 21u) {
                return append_literal(out, out_size, used, "true");
            }
            if (additional == 22u) {
                return append_literal(out, out_size, used, "null");
            }
            if (additional == 27u && reader->length - reader->offset >= 8u) {
                reader->offset += 8u;
                return append_literal(out, out_size, used, "0.0");
            }
            return 0;
        default:
            return 0;
    }
}

static int cbor_to_json(const unsigned char *body, size_t body_size, char *out, size_t out_size)
{
    cbor_reader_t reader = {.data = body, .length = body_size, .offset = 0u};
    size_t used = 0u;
    if (out_size == 0u || !cbor_to_json_value(&reader, out, out_size, &used) || reader.offset != reader.length) {
        if (out_size > 0u) {
            out[0] = '\0';
        }
        return 0;
    }
    return 1;
}

typedef struct fake_transport_context {
    long response_code;
    long next_response_code;
    int switch_after_calls;
    volatile int calls;
    char last_url[256];
    char last_api_key[128];
    char last_payload[16384];
    char last_content_encoding[32];
} fake_transport_context_t;

static honch_status_t fake_transport(
    const char *url,
    const char *api_key,
    const unsigned char *body,
    size_t body_size,
    const char *content_encoding,
    void *userdata,
    long *http_status)
{
    fake_transport_context_t *context = (fake_transport_context_t *)userdata;
    context->calls++;
    long response_code = context->response_code;
    if (context->switch_after_calls > 0 && context->calls > context->switch_after_calls) {
        response_code = context->next_response_code;
    }
    snprintf(context->last_url, sizeof(context->last_url), "%s", url);
    snprintf(context->last_api_key, sizeof(context->last_api_key), "%s", api_key);
    snprintf(context->last_content_encoding, sizeof(context->last_content_encoding), "%s", content_encoding);
    if (!cbor_to_json(body, body_size, context->last_payload, sizeof(context->last_payload))) {
        context->last_payload[0] = '\0';
    }
    *http_status = response_code;
    return HONCH_OK;
}

static int wait_for_transport_calls(fake_transport_context_t *context, int calls)
{
    for (int attempt = 0; attempt < 200; attempt++) {
        if (context->calls >= calls) {
            return 1;
        }
        usleep(10000u);
    }
    return 0;
}

static int wait_for_file_count(const char *directory, const char *suffix, size_t count)
{
    for (int attempt = 0; attempt < 200; attempt++) {
        if (count_files_with_suffix(directory, suffix) == count) {
            return 1;
        }
        usleep(10000u);
    }
    return 0;
}

static int test_battery_level = -1;

static int test_battery_callback(void)
{
    return test_battery_level;
}

static int test_battery_sequence[8];
static size_t test_battery_sequence_count = 0u;
static size_t test_battery_sequence_index = 0u;

static int test_battery_sequence_callback(void)
{
    if (test_battery_sequence_index >= test_battery_sequence_count) {
        return -1;
    }
    return test_battery_sequence[test_battery_sequence_index++];
}

static int test_auto_properties_calls = 0;

static honch_status_t test_auto_properties_success(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    int *rssi = (int *)userdata;
    char rssi_json[16];
    snprintf(rssi_json, sizeof(rssi_json), "%d", *rssi);
    test_auto_properties_calls++;

    honch_status_t status = sink(sink_ctx, "$wifi_rssi", rssi_json);
    if (status == HONCH_OK) {
        status = sink(sink_ctx, "adapter_property", "\"adapter-value\"");
    }
    return status;
}

static honch_status_t test_auto_properties_invalid(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    (void)userdata;
    test_auto_properties_calls++;
    return sink(sink_ctx, "$wifi_rssi", "\"unterminated");
}

static honch_status_t test_auto_properties_spoof_reserved(
    void *userdata,
    honch_property_sink_fn sink,
    void *sink_ctx)
{
    (void)userdata;
    test_auto_properties_calls++;

    honch_status_t status = sink(sink_ctx, "$device_id", "\"spoofed-device\"");
    if (status == HONCH_OK) {
        status = sink(sink_ctx, "$sdk_platform", "\"spoofed-platform\"");
    }
    if (status == HONCH_OK) {
        status = sink(sink_ctx, "$wifi_rssi", "-67");
    }
    return status;
}

static honch_config_t test_config(const char *queue_dir)
{
    honch_config_t config = {
        .api_key = "test-key",
        .endpoint_url = "http://collector.local/",
        .device_id = "device-1",
        .device_model = "X3-Pro",
        .firmware_version = "3.4.1",
        .environment = "production",
        .queue_directory = queue_dir,
        .batch_size = 2u,
        .max_queued_events = 10u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 1000u,
        .disable_background_flush = 1
    };
    return config;
}

static void test_init_validation(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));

    honch_client_t *client = NULL;
    honch_config_t config = test_config(NULL);

    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(client == NULL);

    config = test_config(queue_dir);
    config.device_model = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(client == NULL);

    config = test_config(queue_dir);
    config.firmware_version = "";
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(client == NULL);
}

static void test_esp_idf_status_aliases(void)
{
    EXPECT_EQ_INT(HONCH_ERR_INVALID_ARG, HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(HONCH_ERR_NOT_INITIALIZED, HONCH_ERROR_NOT_INITIALIZED);
    EXPECT_EQ_INT(HONCH_ERR_ALREADY_INITIALIZED, HONCH_ERROR_ALREADY_INITIALIZED);
    EXPECT_EQ_INT(HONCH_ERR_NO_MEM, HONCH_ERROR_OUT_OF_MEMORY);
    EXPECT_EQ_INT(HONCH_ERR_QUEUE_FULL, HONCH_ERROR_QUEUE_FULL);
    EXPECT_EQ_INT(HONCH_ERR_NVS, HONCH_ERROR_IO);
    EXPECT_EQ_INT(HONCH_ERR_TRANSPORT, HONCH_ERROR_TRANSPORT);
    EXPECT_EQ_INT(HONCH_ERR_TIMEOUT, HONCH_ERROR_TIMEOUT);
    EXPECT_EQ_INT(HONCH_ERR_INTERNAL, HONCH_ERROR_INTERNAL);
    EXPECT_TRUE(strcmp(honch_status_string(HONCH_ERROR_NOT_INITIALIZED), "not initialized") == 0);
    EXPECT_TRUE(strcmp(honch_status_string(HONCH_ERROR_ALREADY_INITIALIZED), "already initialized") == 0);
    EXPECT_TRUE(strcmp(honch_status_string(HONCH_ERROR_QUEUE_FULL), "queue full") == 0);
    EXPECT_TRUE(strcmp(honch_status_string(HONCH_ERROR_TIMEOUT), "timeout") == 0);
    EXPECT_TRUE(strcmp(honch_status_string(HONCH_ERROR_INTERNAL), "internal error") == 0);
}

static void test_shutdown_reports_status(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    EXPECT_EQ_INT(honch_shutdown(NULL), HONCH_ERROR_NOT_INITIALIZED);

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_shutdown(client), HONCH_OK);
    honch_test_set_transport(NULL, NULL);
}

static void test_track_persists_event(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "button_pressed", "{\"button\":\"power\"}"), HONCH_OK);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 2);

    honch_shutdown(client);
}

static void test_strict_json_validation(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.max_event_bytes = 512u;

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "bad_event", "[]"), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_track(client, "bad_event", "{\"unterminated\""), HONCH_ERROR_INVALID_ARGUMENT);

    char deep_json[256];
    size_t pos = 0u;
    memcpy(deep_json + pos, "{\"x\":", 5u);
    pos += 5u;
    for (size_t i = 0u; i < 80u; i++) {
        deep_json[pos++] = '[';
    }
    deep_json[pos++] = '0';
    for (size_t i = 0u; i < 80u; i++) {
        deep_json[pos++] = ']';
    }
    deep_json[pos++] = '}';
    deep_json[pos] = '\0';
    EXPECT_EQ_INT(honch_track(client, "deep_event", deep_json), HONCH_ERROR_INVALID_ARGUMENT);

    char oversized_json[640];
    memcpy(oversized_json, "{\"x\":\"", 6u);
    memset(oversized_json + 6u, 'a', 600u);
    oversized_json[606] = '"';
    oversized_json[607] = '}';
    oversized_json[608] = '\0';
    EXPECT_EQ_INT(honch_track(client, "large_event", oversized_json), HONCH_ERROR_INVALID_ARGUMENT);

    EXPECT_EQ_INT(honch_set_property(client, "modes", "[\"hdr\",\"night\"]"), HONCH_OK);
    EXPECT_EQ_INT(honch_set_property(client, "bad", "\"unterminated"), HONCH_ERROR_INVALID_ARGUMENT);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 2);

    honch_shutdown(client);
}

static void test_generated_device_id_persists(void)
{
    char queue_dir[128];
    char device_file[180];
    char first_id[80];
    char second_id[80];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(device_file, sizeof(device_file), "%s/state/device_id", queue_dir);

    honch_config_t config = test_config(queue_dir);
    config.device_id = NULL;

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_TRUE(honch_get_device_id(client) != NULL);
    EXPECT_TRUE(read_text_file(device_file, first_id, sizeof(first_id)) != 0);
    EXPECT_TRUE(strlen(first_id) == 32u);
    EXPECT_TRUE(strcmp(honch_get_device_id(client), first_id) == 0);
    honch_shutdown(client);

    client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_TRUE(read_text_file(device_file, second_id, sizeof(second_id)) != 0);
    EXPECT_TRUE(strcmp(first_id, second_id) == 0);
    EXPECT_TRUE(strcmp(honch_get_device_id(client), second_id) == 0);
    honch_shutdown(client);
}

static void test_existing_invalid_state_path_fails_init(void)
{
    char queue_dir[128];
    char state_dir[180];
    char device_path[220];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(state_dir, sizeof(state_dir), "%s/state", queue_dir);
    snprintf(device_path, sizeof(device_path), "%s/device_id", state_dir);
    EXPECT_EQ_INT(mkdir(state_dir, 0700), 0);
    EXPECT_EQ_INT(mkdir(device_path, 0700), 0);

    honch_config_t config = test_config(queue_dir);
    config.device_id = NULL;

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_IO);
    EXPECT_TRUE(client == NULL);
}

static void test_configured_device_id_accessor(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    char copied_id[16];
    EXPECT_TRUE(honch_get_device_id(NULL) == NULL);
    EXPECT_EQ_INT(honch_copy_device_id(NULL, copied_id, sizeof(copied_id)), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_copy_device_id(client, NULL, sizeof(copied_id)), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_copy_device_id(client, copied_id, 0u), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_TRUE(strcmp(honch_get_device_id(client), "device-1") == 0);
    EXPECT_EQ_INT(honch_copy_device_id(client, copied_id, 4u), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(copied_id[0] == '\0');
    EXPECT_EQ_INT(honch_copy_device_id(client, copied_id, sizeof(copied_id)), HONCH_OK);
    EXPECT_TRUE(strcmp(copied_id, "device-1") == 0);

    honch_shutdown(client);
}

static void test_set_property_emits_event_and_autostamp_conflicts_win(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_set_property(client, "screen_group", "\"diagnostics\""), HONCH_OK);
    EXPECT_EQ_INT(honch_set_property(client, "nullable", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$set_property\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"screen_group\":\"diagnostics\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"nullable\":null");

    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(
        honch_track(client, "screen_viewed", "{\"screen\":\"diagnostics\",\"$device_id\":\"spoofed\",\"\\u0024sdk_platform\":\"spoofed\"}"),
        HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"screen_group\":\"diagnostics\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$device_id\":\"device-1\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"$device_id\":\"spoofed\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$device_model\":\"X3-Pro\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$firmware_version\":\"3.4.1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$sdk_platform\":\"c-posix\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"$sdk_platform\":\"spoofed\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"$sdk_name\":");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"uuid\":");
    long long timestamp = 0;
    EXPECT_TRUE(extract_timestamp_millis(transport.last_payload, &timestamp) != 0);
    EXPECT_TRUE(timestamp > 0);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_auto_properties_callback_adds_platform_properties(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    int rssi = -62;
    test_auto_properties_calls = 0;

    honch_config_t config = test_config(queue_dir);
    config.auto_properties_callback = test_auto_properties_success;
    config.auto_properties_userdata = &rssi;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "platform_event", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_TRUE(test_auto_properties_calls >= 2);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"platform_event\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$wifi_rssi\":-62");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"adapter_property\":\"adapter-value\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_auto_properties_callback_rejects_invalid_json_value(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    test_auto_properties_calls = 0;

    honch_config_t config = test_config(queue_dir);
    config.auto_properties_callback = test_auto_properties_invalid;

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(client == NULL);
    EXPECT_EQ_INT(test_auto_properties_calls, 1);
}

static void test_auto_properties_callback_cannot_override_sdk_owned_properties(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    test_auto_properties_calls = 0;

    honch_config_t config = test_config(queue_dir);
    config.auto_properties_callback = test_auto_properties_spoof_reserved;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "reserved_spoof_attempt", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"reserved_spoof_attempt\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$wifi_rssi\":-67");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$device_id\":\"device-1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$sdk_platform\":\"c-posix\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "spoofed-device");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "spoofed-platform");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_session_events_and_context(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_session_start(NULL, "recording"), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_session_end(NULL), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_session_end(client), HONCH_OK);
    EXPECT_EQ_INT(honch_session_start(client, "recording"), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "recording_started", "{\"mode\":\"hdr\"}"), HONCH_OK);
    EXPECT_EQ_INT(honch_session_end(client), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$session_start\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"recording_started\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$session_end\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"session_name\":\"recording\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$session_id\":\"sess_");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_lifecycle_events_are_queued(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_boot\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"reset_reason\":\"unknown\"");

    EXPECT_EQ_INT(honch_reset(client), HONCH_OK);
    int calls_after_reset = transport.calls;
    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_EQ_INT(transport.calls, calls_after_reset);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$device_reset\"");

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_shutdown(client), HONCH_OK);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_shutdown\"");

    honch_test_set_transport(NULL, NULL);
}

static void test_shutdown_flush_reports_transport_error(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 500L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "queued_before_shutdown", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_shutdown(client), HONCH_ERROR_SERVER);
    EXPECT_TRUE(transport.calls > 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"queued_before_shutdown\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_shutdown\"");

    honch_test_set_transport(NULL, NULL);
}

static void test_firmware_update_emitted_when_version_changes(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.firmware_version = "1.0.0";

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$firmware_update\"");
    honch_shutdown(client);

    transport.last_payload[0] = '\0';
    config.firmware_version = "1.1.0";
    client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$firmware_update\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"previous_version\":\"1.0.0\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"new_version\":\"1.1.0\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_battery_callback_stamps_level_and_emits_low_event(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.battery_callback = test_battery_callback;
    config.battery_low_threshold = 20;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    test_battery_level = 87;
    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "battery_nominal", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$battery_level\":87");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$battery_low\"");

    test_battery_level = 12;
    EXPECT_EQ_INT(honch_track(client, "battery_sample", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$battery_low\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"level\":12");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$battery_level\":12");
    EXPECT_EQ_INT(count_substring(transport.last_payload, "\"event\":\"$battery_low\""), 1);

    test_battery_level = 10;
    EXPECT_EQ_INT(honch_track(client, "battery_still_low", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$battery_low\"");

    test_battery_level = 55;
    EXPECT_EQ_INT(honch_track(client, "battery_recovered", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$battery_low\"");

    test_battery_level = 9;
    EXPECT_EQ_INT(honch_track(client, "battery_low_again", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$battery_low\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"level\":9");

    honch_shutdown(client);
    test_battery_level = -1;
    honch_test_set_transport(NULL, NULL);
}

static void test_battery_low_uses_same_sample_for_event_properties(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.battery_callback = test_battery_sequence_callback;
    config.battery_low_threshold = 20;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    test_battery_sequence[0] = 87;
    test_battery_sequence[1] = 10;
    test_battery_sequence[2] = 80;
    test_battery_sequence_count = 3u;
    test_battery_sequence_index = 0u;

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "battery_sample", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    const char *low_event = strstr(transport.last_payload, "\"event\":\"$battery_low\"");
    EXPECT_TRUE(low_event != NULL);
    if (low_event != NULL) {
        EXPECT_STR_CONTAINS(low_event, "\"level\":10");
        EXPECT_STR_CONTAINS(low_event, "\"$battery_level\":10");
        EXPECT_STR_NOT_CONTAINS(low_event, "\"$battery_level\":80");
    }

    honch_shutdown(client);
    test_battery_sequence_count = 0u;
    test_battery_sequence_index = 0u;
    honch_test_set_transport(NULL, NULL);
}

static void test_identify_payload_and_persistence(void)
{
    char queue_dir[128];
    char distinct_file[180];
    char distinct_id[80];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(distinct_file, sizeof(distinct_file), "%s/state/distinct_id", queue_dir);
    honch_config_t config = test_config(queue_dir);

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_identify(client, "user-1", "{\"plan\":\"beta\"}"), HONCH_OK);
    EXPECT_TRUE(read_text_file(distinct_file, distinct_id, sizeof(distinct_id)) != 0);
    EXPECT_TRUE(strcmp(distinct_id, "user-1") == 0);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$identify\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"distinct_id\":\"user-1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"plan\":\"beta\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"$anon_distinct_id\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"$set\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_identify_does_not_queue_event_when_persistence_fails(void)
{
    char queue_dir[128];
    char state_dir[160];
    char pending_dir[160];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(state_dir, sizeof(state_dir), "%s/state", queue_dir);
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 1);
    EXPECT_EQ_INT(chmod(state_dir, 0500), 0);

    EXPECT_EQ_INT(honch_identify(client, "user-1", "{\"plan\":\"beta\"}"), HONCH_ERROR_IO);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 1);

    EXPECT_EQ_INT(chmod(state_dir, 0700), 0);
    honch_shutdown(client);
}

static void test_queue_limit_drops_oldest(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.max_queued_events = 1u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "first", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "second", NULL), HONCH_OK);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 1);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"first\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"second\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_retry_keeps_events(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.endpoint_url = "http://127.0.0.1:1";

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "first", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_TRANSPORT);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 2);

    honch_shutdown(client);
}

static void test_flush_retryable_http_status_keeps_events_until_success(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 429L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "retryable_status", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_RATE_LIMITED);

    char pending_dir[160];
    char dead_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    snprintf(dead_dir, sizeof(dead_dir), "%s/dead", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 2);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 0);

    transport.response_code = 202L;
    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"retryable_status\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_dead_letters_invalid_persisted_queue_files(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.max_event_bytes = 512u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "valid_after_corrupt", NULL), HONCH_OK);

    char pending_dir[160];
    char dead_dir[160];
    char malformed_path[240];
    char oversized_path[240];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    snprintf(dead_dir, sizeof(dead_dir), "%s/dead", queue_dir);
    snprintf(malformed_path, sizeof(malformed_path), "%s/00000000000000000000-malformed.cbor", pending_dir);
    snprintf(oversized_path, sizeof(oversized_path), "%s/00000000000000000001-oversized.cbor", pending_dir);

    EXPECT_TRUE(write_text_file(malformed_path, "{\"event\":") != 0);

    char oversized[700];
    const char prefix[] = "{\"event\":\"oversized\",\"properties\":{\"x\":\"";
    size_t pos = sizeof(prefix) - 1u;
    memcpy(oversized, prefix, pos);
    memset(oversized + pos, 'a', 620u);
    pos += 620u;
    oversized[pos++] = '"';
    oversized[pos++] = '}';
    oversized[pos++] = '}';
    oversized[pos] = '\0';
    EXPECT_TRUE(write_text_file(oversized_path, oversized) != 0);

    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_REJECTED);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 2);
    EXPECT_EQ_INT(transport.calls, 1);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"valid_after_corrupt\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_dead_letters_semantically_invalid_cbor_event(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);

    char pending_dir[160];
    char dead_dir[160];
    char invalid_path[240];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    snprintf(dead_dir, sizeof(dead_dir), "%s/dead", queue_dir);
    snprintf(invalid_path, sizeof(invalid_path), "%s/00000000000000000000-invalid.cbor", pending_dir);

    const unsigned char valid_but_not_event[] = {0xf6};
    EXPECT_TRUE(write_bytes_file(invalid_path, valid_but_not_event, sizeof(valid_but_not_event)) != 0);
    EXPECT_EQ_INT(honch_track(client, "valid_after_invalid", "{\"ok\":true}"), HONCH_OK);

    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_REJECTED);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 1);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"valid_after_invalid\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_cbor_encoding_preserves_json_string_escapes(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "string_escape_event", "{\"accent\":\"\\u00e9\",\"nul\":\"a\\u0000b\"}"), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_CONTAINS(transport.last_payload, "\"accent\":\"\xc3\xa9\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"nul\":\"a\\u0000b\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"accent\":\"?\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_background_flush_threshold_drains_queue(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.disable_background_flush = 0;
    config.flush_event_threshold = 2u;
    config.flush_retry_initial_ms = 10u;
    config.flush_retry_max_ms = 20u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "threshold_event", NULL), HONCH_OK);
    EXPECT_TRUE(wait_for_transport_calls(&transport, 1) != 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_boot\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"threshold_event\"");

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_TRUE(wait_for_file_count(pending_dir, ".cbor", 0u) != 0);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_background_flush_retries_with_backoff(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;
    config.disable_background_flush = 0;
    config.flush_event_threshold = 2u;
    config.flush_retry_initial_ms = 10u;
    config.flush_retry_max_ms = 20u;

    fake_transport_context_t transport = {
        .response_code = 500L,
        .next_response_code = 202L,
        .switch_after_calls = 1
    };
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "retry_event", NULL), HONCH_OK);
    EXPECT_TRUE(wait_for_transport_calls(&transport, 2) != 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"retry_event\"");

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_TRUE(wait_for_file_count(pending_dir, ".cbor", 0u) != 0);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_background_flush_uses_esp_idf_default_threshold(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 50u;
    config.max_queued_events = 100u;
    config.disable_background_flush = 0;
    config.flush_retry_initial_ms = 10u;
    config.flush_retry_max_ms = 20u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    for (int i = 0; i < 29; i++) {
        EXPECT_EQ_INT(honch_track(client, "default_threshold_event", NULL), HONCH_OK);
    }

    EXPECT_TRUE(wait_for_transport_calls(&transport, 1) != 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"default_threshold_event\"");

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_TRUE(wait_for_file_count(pending_dir, ".cbor", 0u) != 0);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_background_flush_can_be_explicitly_disabled(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 50u;
    config.max_queued_events = 100u;
    config.disable_background_flush = 1;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    for (int i = 0; i < 35; i++) {
        EXPECT_EQ_INT(honch_track(client, "disabled_background_event", NULL), HONCH_OK);
    }

    usleep(50000u);
    EXPECT_EQ_INT(transport.calls, 0);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 36);

    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_TRUE(transport.calls > 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"disabled_background_event\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_drains_multiple_batches(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 2u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "first", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "second", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "third", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(transport.calls, 2);
    EXPECT_TRUE(strcmp(transport.last_url, "http://collector.local/batch") == 0);
    EXPECT_TRUE(strcmp(transport.last_api_key, "test-key") == 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"token\":\"test-key\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"third\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_batch_size_is_capped_to_esp_limit(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 100u;
    config.max_queued_events = 100u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    for (int i = 0; i < 60; i++) {
        EXPECT_EQ_INT(honch_track(client, "cap_event", NULL), HONCH_OK);
    }
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(transport.calls, 2);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_sends_raw_cbor_payload(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_EQ_INT(transport.calls, 1);
    EXPECT_TRUE(strcmp(transport.last_content_encoding, "identity") == 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_boot\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_flush_rejected_moves_events_to_dead_letter(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 400L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "rejected", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_REJECTED);

    char pending_dir[160];
    char dead_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    snprintf(dead_dir, sizeof(dead_dir), "%s/dead", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 2);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_reset_clears_queued_events(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);
    config.batch_size = 10u;

    fake_transport_context_t transport = {.response_code = 400L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "stale_event", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_ERROR_REJECTED);

    char pending_dir[160];
    char dead_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    snprintf(dead_dir, sizeof(dead_dir), "%s/dead", queue_dir);
    EXPECT_TRUE(count_files_with_suffix(dead_dir, ".cbor") > 0u);

    transport.response_code = 202L;
    EXPECT_EQ_INT(honch_reset(client), HONCH_OK);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".cbor"), 0);
    int calls_after_reset = transport.calls;
    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_EQ_INT(transport.calls, calls_after_reset);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$device_reset\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"stale_event\"");
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$device_boot\"");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

static void test_reset_does_not_queue_event_when_state_reset_fails(void)
{
    char queue_dir[128];
    char state_dir[160];
    char pending_dir[160];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(state_dir, sizeof(state_dir), "%s/state", queue_dir);
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 1);
    EXPECT_EQ_INT(chmod(state_dir, 0500), 0);

    EXPECT_EQ_INT(honch_reset(client), HONCH_ERROR_IO);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".cbor"), 1);

    EXPECT_EQ_INT(chmod(state_dir, 0700), 0);
    honch_shutdown(client);
}

static void test_reset_generates_new_identity_and_clears_properties(void)
{
    char queue_dir[128];
    char device_file[180];
    char old_id[80];
    char new_id[80];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    snprintf(device_file, sizeof(device_file), "%s/state/device_id", queue_dir);
    honch_config_t config = test_config(queue_dir);
    config.device_id = NULL;

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_TRUE(read_text_file(device_file, old_id, sizeof(old_id)) != 0);
    EXPECT_EQ_INT(honch_set_property(client, "legacy_context", "\"session-1\""), HONCH_OK);
    EXPECT_EQ_INT(honch_reset(client), HONCH_OK);
    EXPECT_TRUE(read_text_file(device_file, new_id, sizeof(new_id)) != 0);
    EXPECT_TRUE(strcmp(old_id, new_id) != 0);
    int calls_after_reset = transport.calls;
    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_EQ_INT(transport.calls, calls_after_reset);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "\"event\":\"$device_reset\"");

    transport.last_payload[0] = '\0';
    EXPECT_EQ_INT(honch_track(client, "after_reset", NULL), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_NOT_CONTAINS(transport.last_payload, "session-1");
    EXPECT_STR_CONTAINS(transport.last_payload, new_id);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
}

int main(void)
{
    test_init_validation();
    test_esp_idf_status_aliases();
    test_shutdown_reports_status();
    test_track_persists_event();
    test_strict_json_validation();
    test_generated_device_id_persists();
    test_existing_invalid_state_path_fails_init();
    test_configured_device_id_accessor();
    test_set_property_emits_event_and_autostamp_conflicts_win();
    test_auto_properties_callback_adds_platform_properties();
    test_auto_properties_callback_rejects_invalid_json_value();
    test_auto_properties_callback_cannot_override_sdk_owned_properties();
    test_session_events_and_context();
    test_lifecycle_events_are_queued();
    test_shutdown_flush_reports_transport_error();
    test_firmware_update_emitted_when_version_changes();
    test_battery_callback_stamps_level_and_emits_low_event();
    test_battery_low_uses_same_sample_for_event_properties();
    test_identify_payload_and_persistence();
    test_identify_does_not_queue_event_when_persistence_fails();
    test_queue_limit_drops_oldest();
    test_flush_retry_keeps_events();
    test_flush_retryable_http_status_keeps_events_until_success();
    test_flush_dead_letters_invalid_persisted_queue_files();
    test_flush_dead_letters_semantically_invalid_cbor_event();
    test_cbor_encoding_preserves_json_string_escapes();
    test_background_flush_threshold_drains_queue();
    test_background_flush_retries_with_backoff();
    test_background_flush_uses_esp_idf_default_threshold();
    test_background_flush_can_be_explicitly_disabled();
    test_flush_drains_multiple_batches();
    test_batch_size_is_capped_to_esp_limit();
    test_flush_sends_raw_cbor_payload();
    test_flush_rejected_moves_events_to_dead_letter();
    test_reset_clears_queued_events();
    test_reset_does_not_queue_event_when_state_reset_fails();
    test_reset_generates_new_identity_and_clears_properties();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }

    return 0;
}
