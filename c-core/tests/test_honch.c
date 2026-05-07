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

static int read_first_json_file(const char *directory, char *out, size_t size)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return 0;
    }

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 5u || strcmp(entry->d_name + length - 5u, ".json") != 0) {
            continue;
        }

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        result = read_text_file(path, out, size);
        break;
    }

    closedir(dir);
    return result;
}

typedef struct fake_transport_context {
    long response_code;
    int calls;
    char last_url[256];
    char last_api_key[128];
    char last_payload[16384];
} fake_transport_context_t;

static honch_status_t fake_transport(
    const char *url,
    const char *api_key,
    const char *payload,
    void *userdata,
    long *http_status)
{
    fake_transport_context_t *context = (fake_transport_context_t *)userdata;
    context->calls++;
    snprintf(context->last_url, sizeof(context->last_url), "%s", url);
    snprintf(context->last_api_key, sizeof(context->last_api_key), "%s", api_key);
    snprintf(context->last_payload, sizeof(context->last_payload), "%s", payload);
    *http_status = context->response_code;
    return HONCH_OK;
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
        .transport_timeout_ms = 1000u
    };
    return config;
}

static void test_init_validation(void)
{
    honch_client_t *client = NULL;
    honch_config_t config = test_config(NULL);

    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(client == NULL);
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
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 2);

    honch_shutdown(client);
}

static void test_strict_json_validation(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "bad_event", "[]"), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_track(client, "bad_event", "{\"unterminated\""), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_INT(honch_set_property(client, "modes", "[\"hdr\",\"night\"]"), HONCH_OK);
    EXPECT_EQ_INT(honch_set_property(client, "bad", "\"unterminated"), HONCH_ERROR_INVALID_ARGUMENT);

    char pending_dir[160];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 1);

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

static void test_configured_device_id_accessor(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    honch_client_t *client = NULL;
    EXPECT_TRUE(honch_get_device_id(NULL) == NULL);
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_TRUE(strcmp(honch_get_device_id(client), "device-1") == 0);

    honch_shutdown(client);
}

static void test_set_property_attaches_to_future_events(void)
{
    char queue_dir[128];
    make_temp_dir(queue_dir, sizeof(queue_dir));
    honch_config_t config = test_config(queue_dir);

    fake_transport_context_t transport = {.response_code = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_client_t *client = NULL;
    EXPECT_EQ_INT(honch_init(&client, &config), HONCH_OK);
    EXPECT_EQ_INT(honch_set_property(client, "$session_id", "\"session-1\""), HONCH_OK);
    EXPECT_EQ_INT(honch_track(client, "screen_viewed", "{\"screen\":\"diagnostics\"}"), HONCH_OK);
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);

    EXPECT_STR_CONTAINS(transport.last_payload, "\"$session_id\":\"session-1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$device_id\":\"device-1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$device_model\":\"X3-Pro\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$firmware_version\":\"3.4.1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$sdk_platform\":\"c-posix\"");

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
    EXPECT_EQ_INT(honch_flush(client), HONCH_OK);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"$device_reset\"");

    char pending_dir[160];
    char shutdown_event[4096];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", queue_dir);
    honch_shutdown(client);
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 1);
    EXPECT_TRUE(read_first_json_file(pending_dir, shutdown_event, sizeof(shutdown_event)) != 0);
    EXPECT_STR_CONTAINS(shutdown_event, "\"event\":\"$device_shutdown\"");

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
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$anon_distinct_id\":\"device-1\"");
    EXPECT_STR_CONTAINS(transport.last_payload, "\"$set\":{\"plan\":\"beta\"}");

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
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
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 1);
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
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 2);

    honch_shutdown(client);
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
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 0);
    EXPECT_EQ_INT(transport.calls, 2);
    EXPECT_TRUE(strcmp(transport.last_url, "http://collector.local/v1/batch") == 0);
    EXPECT_TRUE(strcmp(transport.last_api_key, "test-key") == 0);
    EXPECT_STR_CONTAINS(transport.last_payload, "\"event\":\"third\"");

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
    EXPECT_EQ_INT(count_files_with_suffix(pending_dir, ".json"), 0);
    EXPECT_EQ_INT(count_files_with_suffix(dead_dir, ".json"), 2);

    honch_shutdown(client);
    honch_test_set_transport(NULL, NULL);
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
    EXPECT_EQ_INT(honch_set_property(client, "$session_id", "\"session-1\""), HONCH_OK);
    EXPECT_EQ_INT(honch_reset(client), HONCH_OK);
    EXPECT_TRUE(read_text_file(device_file, new_id, sizeof(new_id)) != 0);
    EXPECT_TRUE(strcmp(old_id, new_id) != 0);
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
    test_track_persists_event();
    test_strict_json_validation();
    test_generated_device_id_persists();
    test_configured_device_id_accessor();
    test_set_property_attaches_to_future_events();
    test_session_events_and_context();
    test_lifecycle_events_are_queued();
    test_identify_payload_and_persistence();
    test_queue_limit_drops_oldest();
    test_flush_retry_keeps_events();
    test_flush_drains_multiple_batches();
    test_flush_rejected_moves_events_to_dead_letter();
    test_reset_generates_new_identity_and_clears_properties();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }

    return 0;
}
