#ifndef HONCH_INTERNAL_H
#define HONCH_INTERNAL_H

#include "honch/honch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define HONCH_SDK_VERSION "0.2.0"
#define HONCH_DEFAULT_BATCH_SIZE 20u
#define HONCH_MAX_BATCH_SIZE 50u
#define HONCH_DEFAULT_MAX_QUEUED_EVENTS 1000u
#define HONCH_DEFAULT_MAX_EVENT_BYTES 16384u
#define HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS 10000u
#define HONCH_DEFAULT_FLUSH_INTERVAL_SECONDS 60u
#define HONCH_DEFAULT_FLUSH_EVENT_THRESHOLD 30u
#define HONCH_DEFAULT_BATTERY_LOW_THRESHOLD 15
#define HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS 1000u
#define HONCH_DEFAULT_FLUSH_RETRY_MAX_MS 300000u
#define HONCH_MAX_EVENT_NAME 128u
#define HONCH_MAX_DISTINCT_ID 256u

typedef struct honch_buffer {
    char *data;
    size_t length;
    size_t capacity;
} honch_buffer_t;

typedef struct honch_file_entry {
    char *name;
    char *path;
} honch_file_entry_t;

typedef struct honch_file_list {
    honch_file_entry_t *items;
    size_t count;
    size_t capacity;
} honch_file_list_t;

typedef enum honch_http_result {
    HONCH_HTTP_OK,
    HONCH_HTTP_RETRY,
    HONCH_HTTP_REJECTED
} honch_http_result_t;

struct honch_client {
    pthread_mutex_t mutex;
    pthread_cond_t scheduler_cond;
    pthread_t scheduler_thread;
    char *api_key;
    char *endpoint_url;
    char *device_id;
    char *device_model;
    char *firmware_version;
    char *environment;
    char *queue_directory;
    char *pending_directory;
    char *dead_directory;
    char *state_directory;
    char *distinct_id;
    char *session_id;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    size_t flush_event_threshold;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    uint64_t next_interval_flush_ms;
    uint64_t next_retry_flush_ms;
    unsigned int current_retry_delay_ms;
    bool scheduler_enabled;
    bool scheduler_started;
    bool scheduler_stop;
    bool scheduler_flush_requested;
    int (*battery_callback)(void);
    int battery_low_threshold;
    honch_auto_properties_fn auto_properties_callback;
    void *auto_properties_userdata;
    bool battery_low_emitted;
    uint64_t sequence;
};

bool honch_is_blank(const char *value);
bool honch_property_key_is_reserved(const char *key);
char *honch_strdup(const char *value);
void honch_free_client_fields(honch_client_t *client);
honch_status_t honch_size_add(size_t left, size_t right, size_t *out);
honch_status_t honch_size_add3(size_t first, size_t second, size_t third, size_t *out);
honch_status_t honch_size_mul(size_t left, size_t right, size_t *out);

honch_status_t honch_buffer_init(honch_buffer_t *buffer, size_t initial_capacity);
void honch_buffer_free(honch_buffer_t *buffer);
honch_status_t honch_buffer_reserve(honch_buffer_t *buffer, size_t needed);
honch_status_t honch_buffer_append_n(honch_buffer_t *buffer, const char *value, size_t length);
honch_status_t honch_buffer_append(honch_buffer_t *buffer, const char *value);
honch_status_t honch_buffer_appendf(honch_buffer_t *buffer, const char *format, ...);

honch_status_t honch_json_append_string(honch_buffer_t *buffer, const char *value);
bool honch_json_is_object(const char *json);
bool honch_json_is_value(const char *json);
bool honch_json_object_has_members(const char *json);
honch_status_t honch_json_append_object_members(honch_buffer_t *buffer, const char *json, bool *has_members);

honch_status_t honch_encoder_build_batch_json(
    honch_client_t *client,
    const honch_file_list_t *files,
    size_t count,
    char **out,
    size_t *invalid_index);

uint64_t honch_now_millis(void);
honch_status_t honch_now_iso8601(char out[25]);
honch_status_t honch_random_hex(char out[33]);
honch_status_t honch_join_path(char **out, const char *left, const char *right);
honch_status_t honch_mkdir_p(const char *path);
honch_status_t honch_read_file(const char *path, char **out);
honch_status_t honch_read_file_limited(const char *path, size_t max_bytes, char **out);
honch_status_t honch_write_file_atomic(const char *directory, const char *filename, const char *content);
honch_status_t honch_list_files_with_suffix(const char *directory, const char *suffix, honch_file_list_t *list);
void honch_file_list_free(honch_file_list_t *list);
honch_status_t honch_unlink_if_exists(const char *path);

honch_status_t honch_state_prepare(honch_client_t *client, const honch_config_t *config);
honch_status_t honch_state_save_distinct_id(honch_client_t *client);
honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinct_id);
honch_status_t honch_state_check_firmware_version(honch_client_t *client, bool *changed, char **previous_version);
honch_status_t honch_state_reset(honch_client_t *client);

honch_status_t honch_queue_enqueue(honch_client_t *client, const char *event_json);
honch_status_t honch_queue_clear(honch_client_t *client);
honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count);
honch_status_t honch_queue_flush_locked(honch_client_t *client);

honch_status_t honch_transport_post_batch(
    honch_client_t *client,
    const char *payload,
    honch_http_result_t *result);

#endif
