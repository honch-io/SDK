#include "honch/honch.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct response_buffer {
    char data[256];
    size_t length;
} response_buffer_t;

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

static int clickhouse_count_event(
    const char *clickhouse_url,
    const char *database,
    const char *project_id,
    const char *event_name,
    long *count)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return 0;
    }

    char query[512];
    snprintf(
        query,
        sizeof(query),
        "SELECT count() FROM %s.events WHERE team_id = '%s' AND event = '%s' FORMAT TabSeparated",
        database,
        project_id,
        event_name);

    char *escaped = curl_easy_escape(curl, query, 0);
    if (escaped == NULL) {
        curl_easy_cleanup(curl);
        return 0;
    }

    char url[1024];
    snprintf(url, sizeof(url), "%s/?query=%s", clickhouse_url, escaped);
    curl_free(escaped);

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
        return 0;
    }

    *count = strtol(response.data, NULL, 10);
    return 1;
}

static int wait_for_clickhouse_event(
    const char *clickhouse_url,
    const char *database,
    const char *project_id,
    const char *event_name)
{
    for (int attempt = 0; attempt < 30; attempt++) {
        long count = 0L;
        if (clickhouse_count_event(clickhouse_url, database, project_id, event_name, &count) && count > 0L) {
            return 1;
        }
        sleep(1u);
    }
    return 0;
}

int main(void)
{
    const char *endpoint = getenv("HONCH_E2E_ENDPOINT");
    const char *token = getenv("HONCH_E2E_TOKEN");
    if (endpoint == NULL || endpoint[0] == '\0' || token == NULL || token[0] == '\0') {
        fprintf(stderr, "Skipping: HONCH_E2E_ENDPOINT and HONCH_E2E_TOKEN are required.\n");
        return 77;
    }

    char queue_dir[128];
    snprintf(queue_dir, sizeof(queue_dir), "/tmp/honch-e2e-%ld-XXXXXX", (long)getpid());
    if (mkdtemp(queue_dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    char event_name[96];
    snprintf(event_name, sizeof(event_name), "c_core_e2e_smoke_%ld", (long)getpid());

    honch_config_t config = {
        .api_key = token,
        .endpoint_url = endpoint,
        .device_id = NULL,
        .device_model = "C Core E2E",
        .firmware_version = "e2e",
        .environment = "e2e",
        .queue_directory = queue_dir,
        .batch_size = 10u,
        .max_queued_events = 100u,
        .max_event_bytes = 8192u,
        .transport_timeout_ms = 15000u
    };

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    if (status != HONCH_OK) {
        log_status("honch_init", status);
        return 1;
    }

    status = honch_track(client, event_name, "{\"source\":\"c-core-e2e\"}");
    if (status == HONCH_OK) {
        status = honch_flush(client);
    }

    honch_shutdown(client);

    if (status != HONCH_OK) {
        log_status("E2E capture", status);
        return 1;
    }

    const char *clickhouse_url = getenv("HONCH_E2E_CLICKHOUSE_URL");
    const char *project_id = getenv("HONCH_E2E_PROJECT_ID");
    const char *database = getenv("HONCH_E2E_CLICKHOUSE_DATABASE");
    if (database == NULL || database[0] == '\0') {
        database = "platform";
    }

    if ((clickhouse_url == NULL || clickhouse_url[0] == '\0') &&
        project_id != NULL && project_id[0] != '\0') {
        fprintf(stderr, "HONCH_E2E_CLICKHOUSE_URL is required when HONCH_E2E_PROJECT_ID is set.\n");
        return 1;
    }
    if (clickhouse_url != NULL && clickhouse_url[0] != '\0' &&
        (project_id == NULL || project_id[0] == '\0')) {
        fprintf(stderr, "HONCH_E2E_PROJECT_ID is required when HONCH_E2E_CLICKHOUSE_URL is set.\n");
        return 1;
    }

    if (clickhouse_url != NULL && clickhouse_url[0] != '\0' &&
        project_id != NULL && project_id[0] != '\0') {
        if (!wait_for_clickhouse_event(clickhouse_url, database, project_id, event_name)) {
            fprintf(stderr, "E2E capture event did not appear in ClickHouse: %s\n", event_name);
            return 1;
        }
    }

    return 0;
}
