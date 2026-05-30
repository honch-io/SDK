#ifndef HONCH_INTERNAL_H
#define HONCH_INTERNAL_H

#include "honch/core/honch.h"
#include "honch/core/wire_v2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#define HONCH_MAX_EVENT_PROPERTIES 64u

typedef struct honch_buffer {
    char *data;
    size_t length;
    size_t capacity;
} honch_buffer_t;

typedef struct honch_payload {
    unsigned char *data;
    size_t length;
} honch_payload_t;

typedef struct honch_file_entry {
    char *name;
    char *path;
} honch_file_entry_t;

typedef struct honch_file_list {
    honch_file_entry_t *items;
    size_t count;
    size_t capacity;
} honch_file_list_t;

typedef struct honch_event_record {
    const char *event_name;
    const char *distinct_id;
    const char *session_id;
    uint64_t timestamp_ms;
    honch_wire_v2_property_t properties[64];
    size_t property_count;
} honch_event_record_t;

struct honch_client {
    void *lifetime_mutex;
    void *state_mutex;
    honch_platform_ops_t platform_ops;
    const honch_platform_ops_t *platform;
    honch_storage_ops_t storage_ops;
    const honch_storage_ops_t *storage;
    honch_transport_ops_t transport_ops;
    const honch_transport_ops_t *transport;
    char *api_key;
    char *endpoint_url;
    char *device_id;
    char *device_model;
    char *firmware_version;
    char *environment;
    char *sdk_platform;
    char *queue_directory;
    char *pending_directory;
    char *dead_directory;
    char *state_directory;
    char *distinct_id;
    char *session_id;
    honch_wire_v2_property_t build_properties[HONCH_MAX_EVENT_PROPERTIES];
    bool configured_device_id;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    size_t flush_event_threshold;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    uint32_t wire_v2_message_id_seed;
    char wire_v2_stream_id[9];
    honch_durability_mode_t durability_mode;
    uint64_t next_interval_flush_ms;
    uint64_t next_retry_flush_ms;
    unsigned int current_retry_delay_ms;
    uint64_t active_storage_reader_sequence;
    bool scheduler_flush_requested;
    bool flush_in_progress;
    bool closing;
    size_t active_calls;
    int (*battery_callback)(void);
    int battery_low_threshold;
    honch_auto_properties_fn auto_properties_callback;
    void *auto_properties_userdata;
    bool battery_low_emitted;
    uint64_t sequence;
    size_t queued_event_count;
};

bool honch_is_blank(const char *value);
bool honch_property_key_is_reserved(const char *key);
bool honch_utf8_is_valid(const char *value, size_t length);
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

honch_status_t honch_event_record_build(
    const char *event_name,
    const char *distinct_id,
    const char *session_id,
    uint64_t timestamp_ms,
    const honch_wire_v2_property_t *properties,
    size_t property_count,
    honch_payload_t *out);
honch_status_t honch_event_record_parse(const uint8_t *data, size_t length, honch_event_record_t *record);
void honch_event_record_prepare_wire_properties(honch_event_record_t *record);
void honch_event_record_free(honch_event_record_t *record);
bool honch_event_record_validate(const uint8_t *data, size_t length);

uint64_t honch_now_millis(void);
honch_status_t honch_random_hex(char out[33]);
honch_status_t honch_join_path(char **out, const char *left, const char *right);
honch_status_t honch_mkdir_p(const char *path);
honch_status_t honch_read_file(const char *path, char **out);
honch_status_t honch_read_file_limited(const char *path, size_t max_bytes, char **out);
honch_status_t honch_read_file_limited_bytes(const char *path, size_t max_bytes, honch_payload_t *out);
honch_status_t honch_write_file_atomic(const char *directory, const char *filename, const char *content);
honch_status_t honch_write_file_atomic_bytes(
    const char *directory,
    const char *filename,
    const unsigned char *content,
    size_t content_size);
honch_status_t honch_write_file_atomic_bytes_with_durability(
    const char *directory,
    const char *filename,
    const unsigned char *content,
    size_t content_size,
    honch_durability_mode_t durability_mode);
honch_status_t honch_list_files_with_suffix(const char *directory, const char *suffix, honch_file_list_t *list);
void honch_file_list_free(honch_file_list_t *list);
honch_status_t honch_unlink_if_exists(const char *path);

honch_status_t honch_state_prepare(honch_client_t *client, const honch_core_config_t *config);
honch_status_t honch_state_save_distinct_id(honch_client_t *client);
honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinct_id);
honch_status_t honch_state_check_firmware_version(honch_client_t *client, bool *changed, char **previous_version);
honch_status_t honch_state_save_firmware_version(honch_client_t *client);
honch_status_t honch_state_reset(honch_client_t *client);

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t event_size);
honch_status_t honch_queue_clear(honch_client_t *client);
honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count);
honch_status_t honch_queue_flush_one_locked(honch_client_t *client, bool *progressed);
honch_status_t honch_queue_flush_locked(honch_client_t *client);

honch_status_t honch_client_enter(honch_client_t *client);
void honch_client_leave(honch_client_t *client);
honch_status_t honch_client_begin_shutdown(honch_client_t *client);
bool honch_client_lock_ops_valid(const honch_platform_ops_t *platform);
honch_status_t honch_client_lock_create(honch_client_t *client, void **mutex);
void honch_client_lock_destroy(honch_client_t *client, void *mutex);
honch_status_t honch_client_state_lock(honch_client_t *client);
void honch_client_state_unlock(honch_client_t *client);

#endif
