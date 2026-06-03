#include "honch/honch.h"

#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_CAPTURE_URL "http://127.0.0.1:8001"
#define DEFAULT_CAPTURE_HEALTH_URL "http://127.0.0.1:8001/health"
#define DEFAULT_WORKER_HEALTH_URL "http://127.0.0.1:8080/"
#define DEFAULT_TOKEN "honch_e2e_test_key"
#define DEFAULT_PROJECT_ID "00000000-0000-0000-0000-000000000002"
#define DEFAULT_CLICKHOUSE_URL "http://127.0.0.1:8123"
#define DEFAULT_CLICKHOUSE_DATABASE "platform"

typedef struct response_buffer {
    char data[65536];
    size_t length;
} response_buffer_t;

typedef struct e2e_row {
    char event[128];
    char distinct_id[256];
    char properties[32768];
    char device_id[256];
    char device_model[256];
    char firmware_version[256];
    char session_id[256];
    char sdk_platform[128];
    char environment[128];
} e2e_row_t;

static int failures = 0;
static int e2e_battery_level = -1;
static int e2e_wifi_rssi = -64;

#define EXPECT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)
#define EXPECT_EQ_STATUS(actual, expected) do { honch_status_t av = (actual); honch_status_t ev = (expected); if (av != ev) { fprintf(stderr, "FAIL %s:%d: %s == %s (%s != %s)\n", __FILE__, __LINE__, #actual, #expected, honch_status_string(av), honch_status_string(ev)); failures++; } } while (0)
#define EXPECT_STR_CONTAINS(haystack, needle) EXPECT_TRUE(strstr((haystack), (needle)) != NULL)
#define EXPECT_STR_NOT_CONTAINS(haystack, needle) EXPECT_TRUE(strstr((haystack), (needle)) == NULL)

static const char *env_or_default(const char *name, const char *default_value)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : default_value;
}

static void log_status(const char *operation, honch_status_t status)
{
    fprintf(stderr, "%s failed: %s\n", operation, honch_status_string(status));
}

static size_t write_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    response_buffer_t *buffer = (response_buffer_t *)userdata;
    if (size != 0u && nmemb > ((size_t)-1) / size) {
        return 0u;
    }

    size_t incoming = size * nmemb;
    size_t available = sizeof(buffer->data) - buffer->length - 1u;
    size_t to_copy = incoming < available ? incoming : available;
    if (to_copy > 0u) {
        memcpy(buffer->data + buffer->length, ptr, to_copy);
        buffer->length += to_copy;
        buffer->data[buffer->length] = '\0';
    }

    return incoming;
}

static int http_get_ok(const char *url)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return 0;
    }

    response_buffer_t response = {{0}, 0u};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode code = curl_easy_perform(curl);
    long http_status = 0L;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || http_status < 200L || http_status >= 300L) {
        fprintf(stderr, "Health check failed for %s: curl=%d http=%ld body=%s\n", url, (int)code, http_status, response.data);
        return 0;
    }
    return 1;
}

static int clickhouse_query(const char *clickhouse_url, const char *query, response_buffer_t *response)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return 0;
    }

    char *escaped = curl_easy_escape(curl, query, 0);
    if (escaped == NULL) {
        curl_easy_cleanup(curl);
        return 0;
    }

    char url[4096];
    snprintf(url, sizeof(url), "%s/?query=%s", clickhouse_url, escaped);
    curl_free(escaped);

    memset(response, 0, sizeof(*response));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode code = curl_easy_perform(curl);
    long http_status = 0L;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || http_status < 200L || http_status >= 300L) {
        fprintf(stderr, "ClickHouse query failed: curl=%d http=%ld body=%s\nquery=%s\n", (int)code, http_status, response->data, query);
        return 0;
    }
    return 1;
}

static void copy_field(char *out, size_t out_size, const char *start, size_t length)
{
    size_t count = length < out_size - 1u ? length : out_size - 1u;
    memcpy(out, start, count);
    out[count] = '\0';
    if (strcmp(out, "\\N") == 0) {
        out[0] = '\0';
    }
}

static int parse_row(const char *line, e2e_row_t *row)
{
    char *fields[] = {
        row->event,
        row->distinct_id,
        row->properties,
        row->device_id,
        row->device_model,
        row->firmware_version,
        row->session_id,
        row->sdk_platform,
        row->environment
    };
    size_t sizes[] = {
        sizeof(row->event),
        sizeof(row->distinct_id),
        sizeof(row->properties),
        sizeof(row->device_id),
        sizeof(row->device_model),
        sizeof(row->firmware_version),
        sizeof(row->session_id),
        sizeof(row->sdk_platform),
        sizeof(row->environment)
    };

    memset(row, 0, sizeof(*row));
    const char *cursor = line;
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *end = strchr(cursor, i == 8u ? '\n' : '\t');
        if (end == NULL) {
            end = cursor + strlen(cursor);
        }
        copy_field(fields[i], sizes[i], cursor, (size_t)(end - cursor));
        cursor = *end == '\0' ? end : end + 1;
    }

    return row->event[0] != '\0';
}

static int clickhouse_fetch_event(
    const char *clickhouse_url,
    const char *database,
    const char *project_id,
    const char *event_name,
    e2e_row_t *row)
{
    char query[2048];
    snprintf(
        query,
        sizeof(query),
        "SELECT event, distinct_id, properties, ifNull(device_id, ''), ifNull(device_model, ''), "
        "ifNull(firmware_version, ''), ifNull(session_id, ''), ifNull(sdk_platform, ''), ifNull(environment, '') "
        "FROM %s.events WHERE team_id = '%s' AND event = '%s' ORDER BY received_at DESC LIMIT 1 FORMAT TabSeparated",
        database,
        project_id,
        event_name);

    response_buffer_t response;
    if (!clickhouse_query(clickhouse_url, query, &response) || response.length == 0u) {
        return 0;
    }
    return parse_row(response.data, row);
}

static int wait_for_clickhouse_event(
    const char *clickhouse_url,
    const char *database,
    const char *project_id,
    const char *event_name,
    e2e_row_t *row)
{
    for (int attempt = 0; attempt < 45; attempt++) {
        if (clickhouse_fetch_event(clickhouse_url, database, project_id, event_name, row)) {
            return 1;
        }
        usleep(1000000u);
    }
    fprintf(stderr, "E2E event did not appear in ClickHouse: %s\n", event_name);
    return 0;
}

static int e2e_battery_callback(void)
{
    return e2e_battery_level;
}

static honch_status_t e2e_auto_properties(void *userdata, honch_property_sink_fn sink, void *sink_ctx)
{
    (void)userdata;
    honch_status_t status = sink(sink_ctx, "$wifi_rssi", honch_i64(e2e_wifi_rssi));
    if (status == HONCH_OK) {
        status = sink(sink_ctx, "adapter_property", honch_str("e2e-adapter"));
    }
    if (status == HONCH_OK) {
        status = sink(sink_ctx, "$device_id", honch_str("adapter-spoof"));
    }
    return status;
}

static void make_event_name(char *out, size_t size, const char *prefix, const char *suffix)
{
    snprintf(out, size, "%s_%s", prefix, suffix);
}

static int assert_common_row(const e2e_row_t *row, const char *event_name)
{
    EXPECT_TRUE(strcmp(row->event, event_name) == 0);
    EXPECT_TRUE(strcmp(row->device_model, "C Core E2E") == 0);
    EXPECT_TRUE(strcmp(row->firmware_version, "e2e-fw-1") == 0);
    EXPECT_TRUE(strcmp(row->sdk_platform, "c-posix") == 0);
    EXPECT_TRUE(strcmp(row->environment, "e2e") == 0);
    EXPECT_TRUE(row->device_id[0] != '\0');
    return failures == 0;
}

static int verify_event(
    const char *clickhouse_url,
    const char *database,
    const char *project_id,
    const char *event_name,
    e2e_row_t *row)
{
    if (!wait_for_clickhouse_event(clickhouse_url, database, project_id, event_name, row)) {
        failures++;
        return 0;
    }
    assert_common_row(row, event_name);
    return 1;
}

int main(void)
{
    const char *endpoint = env_or_default("HONCH_E2E_ENDPOINT", DEFAULT_CAPTURE_URL);
    const char *token = env_or_default("HONCH_E2E_TOKEN", DEFAULT_TOKEN);
    const char *capture_health = env_or_default("HONCH_E2E_CAPTURE_HEALTH_URL", DEFAULT_CAPTURE_HEALTH_URL);
    const char *worker_health = env_or_default("HONCH_E2E_WORKER_HEALTH_URL", DEFAULT_WORKER_HEALTH_URL);
    const char *clickhouse_url = env_or_default("HONCH_E2E_CLICKHOUSE_URL", DEFAULT_CLICKHOUSE_URL);
    const char *project_id = env_or_default("HONCH_E2E_PROJECT_ID", DEFAULT_PROJECT_ID);
    const char *database = env_or_default("HONCH_E2E_CLICKHOUSE_DATABASE", DEFAULT_CLICKHOUSE_DATABASE);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!http_get_ok(capture_health) || !http_get_ok(worker_health) || !http_get_ok(clickhouse_url)) {
        curl_global_cleanup();
        return 1;
    }

    char queue_dir[128];
    snprintf(queue_dir, sizeof(queue_dir), "/tmp/honch-e2e-%ld-XXXXXX", (long)getpid());
    if (mkdtemp(queue_dir) == NULL) {
        perror("mkdtemp");
        curl_global_cleanup();
        return 1;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "posix_e2e_%ld_%ld", (long)getpid(), (long)tv.tv_usec);

    char event_edge[128];
    char event_session[128];
    char event_restart[128];
    char event_after_reset[128];
    make_event_name(event_edge, sizeof(event_edge), prefix, "edge");
    make_event_name(event_session, sizeof(event_session), prefix, "session");
    make_event_name(event_restart, sizeof(event_restart), prefix, "restart");
    make_event_name(event_after_reset, sizeof(event_after_reset), prefix, "after_reset");

    char user_id[128];
    snprintf(user_id, sizeof(user_id), "%s_user", prefix);

    honch_config_t config = {
        .api_key = token,
        .endpoint_url = endpoint,
        .device_id = NULL,
        .device_model = "C Core E2E",
        .firmware_version = "e2e-fw-1",
        .environment = "e2e",
        .queue_directory = queue_dir,
        .batch_size = 3u,
        .max_queued_events = 100u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 15000u,
        .flush_min_interval_ms = HONCH_FLUSH_MIN_INTERVAL_DISABLED_MS,
        .battery_callback = e2e_battery_callback,
        .battery_low_threshold = 20,
        .auto_properties_callback = e2e_auto_properties
    };

    e2e_battery_level = 87;
    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        log_status("honch_init", status);
        curl_global_cleanup();
        return 1;
    }

    char first_device_id[128];
    EXPECT_EQ_STATUS(honch_copy_device_id(client, first_device_id, sizeof(first_device_id)), HONCH_OK);
    EXPECT_TRUE(first_device_id[0] != '\0');

    EXPECT_EQ_STATUS(honch_track(client, "", NULL, 0u), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_STATUS(honch_track(client, event_edge, NULL, 1u), HONCH_ERROR_INVALID_ARGUMENT);
    const honch_property_t invalid_properties[] = {
        honch_prop("bad", honch_f64(NAN))
    };
    EXPECT_EQ_STATUS(honch_track(client, event_edge, invalid_properties, 1u), HONCH_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ_STATUS(honch_set_property(client, "bad", honch_f64(NAN)), HONCH_ERROR_INVALID_ARGUMENT);

    e2e_battery_level = 12;
    const honch_value_t frame_values[] = {
        honch_u64(1),
        honch_u64(2),
        honch_u64(3)
    };
    const honch_map_pair_t nested_pairs[] = {
        honch_pair("mode", honch_str("hdr")),
        honch_pair("frames", honch_array(frame_values, 3))
    };
    const honch_property_t edge_properties[] = {
        honch_prop("source", honch_str("posix-e2e")),
        honch_prop("nested", honch_map(nested_pairs, 2)),
        honch_prop("quote", honch_str("say \"hi\""))
    };
    EXPECT_EQ_STATUS(honch_track(client, event_edge, edge_properties, 3u), HONCH_OK);

    const honch_property_t identify_traits[] = {
        honch_prop("plan", honch_str("beta")),
        honch_prop("cohort", honch_str("local"))
    };
    EXPECT_EQ_STATUS(honch_identify(client, user_id, identify_traits, 2u), HONCH_OK);
    EXPECT_EQ_STATUS(honch_set_property(client, "favorite_mode", honch_str("night")), HONCH_OK);
    EXPECT_EQ_STATUS(honch_session_start(client, "recording"), HONCH_OK);
    const honch_property_t session_properties[] = {
        honch_prop("mode", honch_str("hdr"))
    };
    EXPECT_EQ_STATUS(honch_track(client, event_session, session_properties, 1u), HONCH_OK);
    EXPECT_EQ_STATUS(honch_session_end(client), HONCH_OK);
    EXPECT_EQ_STATUS(honch_flush(client), HONCH_OK);

    e2e_row_t row;
    if (verify_event(clickhouse_url, database, project_id, event_edge, &row)) {
        EXPECT_STR_CONTAINS(row.properties, "\"source\":\"posix-e2e\"");
        EXPECT_STR_CONTAINS(row.properties, "\"adapter_property\":\"e2e-adapter\"");
        EXPECT_STR_CONTAINS(row.properties, "\"$wifi_rssi\":-64");
        EXPECT_STR_CONTAINS(row.properties, "\"$battery_level\":12");
        EXPECT_STR_NOT_CONTAINS(row.properties, "spoofed-device");
        EXPECT_STR_NOT_CONTAINS(row.properties, "spoofed-platform");
        EXPECT_TRUE(strcmp(row.device_id, first_device_id) == 0);
    }

    if (verify_event(clickhouse_url, database, project_id, "$battery_low", &row)) {
        EXPECT_STR_CONTAINS(row.properties, "\"level\":12");
        EXPECT_STR_CONTAINS(row.properties, "\"$battery_level\":12");
    }

    if (verify_event(clickhouse_url, database, project_id, "$identify", &row)) {
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
        EXPECT_STR_CONTAINS(row.properties, "\"plan\":\"beta\"");
        EXPECT_STR_CONTAINS(row.properties, "\"cohort\":\"local\"");
    }

    if (verify_event(clickhouse_url, database, project_id, "$set_property", &row)) {
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
        EXPECT_STR_CONTAINS(row.properties, "\"favorite_mode\":\"night\"");
    }

    if (verify_event(clickhouse_url, database, project_id, "$session_start", &row)) {
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
        EXPECT_TRUE(row.session_id[0] != '\0');
        EXPECT_STR_CONTAINS(row.properties, "\"session_name\":\"recording\"");
    }

    if (verify_event(clickhouse_url, database, project_id, event_session, &row)) {
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
        EXPECT_TRUE(row.session_id[0] != '\0');
        EXPECT_STR_CONTAINS(row.properties, "\"mode\":\"hdr\"");
    }

    if (verify_event(clickhouse_url, database, project_id, "$session_end", &row)) {
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
        EXPECT_TRUE(row.session_id[0] != '\0');
    }

    EXPECT_EQ_STATUS(honch_shutdown(client), HONCH_OK);

    client = NULL;
    e2e_battery_level = 55;
    EXPECT_EQ_STATUS(honch_init(&client, &config), HONCH_OK);

    char restarted_device_id[128];
    EXPECT_EQ_STATUS(honch_copy_device_id(client, restarted_device_id, sizeof(restarted_device_id)), HONCH_OK);
    EXPECT_TRUE(strcmp(restarted_device_id, first_device_id) == 0);
    const honch_property_t restart_properties[] = {
        honch_prop("phase", honch_str("restart"))
    };
    EXPECT_EQ_STATUS(honch_track(client, event_restart, restart_properties, 1u), HONCH_OK);
    EXPECT_EQ_STATUS(honch_flush(client), HONCH_OK);

    if (verify_event(clickhouse_url, database, project_id, event_restart, &row)) {
        EXPECT_TRUE(strcmp(row.device_id, first_device_id) == 0);
        EXPECT_TRUE(strcmp(row.distinct_id, user_id) == 0);
    }

    EXPECT_EQ_STATUS(honch_reset(client), HONCH_OK);

    char reset_device_id[128];
    EXPECT_EQ_STATUS(honch_copy_device_id(client, reset_device_id, sizeof(reset_device_id)), HONCH_OK);
    EXPECT_TRUE(strcmp(reset_device_id, first_device_id) != 0);
    const honch_property_t after_reset_properties[] = {
        honch_prop("phase", honch_str("after_reset"))
    };
    EXPECT_EQ_STATUS(honch_track(client, event_after_reset, after_reset_properties, 1u), HONCH_OK);
    EXPECT_EQ_STATUS(honch_flush(client), HONCH_OK);

    if (verify_event(clickhouse_url, database, project_id, event_after_reset, &row)) {
        EXPECT_TRUE(strcmp(row.device_id, reset_device_id) == 0);
        EXPECT_TRUE(strcmp(row.distinct_id, reset_device_id) == 0);
        EXPECT_STR_NOT_CONTAINS(row.properties, "recording");
    }

    EXPECT_EQ_STATUS(honch_shutdown(client), HONCH_OK);

    curl_global_cleanup();
    if (failures != 0) {
        fprintf(stderr, "%d E2E failure(s)\n", failures);
        return 1;
    }
    return 0;
}
