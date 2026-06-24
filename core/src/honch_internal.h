#ifndef HONCH_INTERNAL_H
#define HONCH_INTERNAL_H

#include "honch/core/honch.h"
#include "honch/core/coredump.h"
#include "honch/core/wire_v2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HONCH_SDK_VERSION "0.2.4"
#define HONCH_DEFAULT_BATCH_SIZE 20u
#ifndef HONCH_MAX_BATCH_SIZE
#define HONCH_MAX_BATCH_SIZE 50u
#endif
#define HONCH_DEFAULT_MAX_QUEUED_EVENTS 1000u
#define HONCH_DEFAULT_MAX_EVENT_BYTES 8192u
#define HONCH_DEFAULT_TRANSPORT_TIMEOUT_MS 2500u
#define HONCH_DEFAULT_FLUSH_INTERVAL_SECONDS 120u
#define HONCH_DEFAULT_FLUSH_MIN_INTERVAL_MS 15000u
#define HONCH_DEFAULT_FLUSH_EVENT_THRESHOLD 20u
#define HONCH_DEFAULT_FLUSH_MAX_BATCHES 1u
#define HONCH_DEFAULT_SHUTDOWN_FLUSH_MAX_BATCHES 1u
#define HONCH_TICK_MAX_CHUNKS 1u
#define HONCH_DEFAULT_BATTERY_LOW_THRESHOLD 15
#define HONCH_DEFAULT_FLUSH_RETRY_INITIAL_MS 1000u
#define HONCH_DEFAULT_FLUSH_RETRY_MAX_MS 300000u
#define HONCH_MIN_UNIX_TIME_MS 1577836800000ULL
#define HONCH_MAX_EVENT_NAME 128u
#define HONCH_MAX_DISTINCT_ID 256u
#ifndef HONCH_MAX_EVENT_PROPERTIES
#define HONCH_MAX_EVENT_PROPERTIES 64u
#endif
/* Auto (global) properties come from the user-registered auto_properties_callback
 * and are capped here; tuning this down shrinks the client's auto-property scratch
 * independently of the per-event property cap. Defaults to the event cap so the
 * default build is unchanged. */
#ifndef HONCH_MAX_AUTO_PROPERTIES
#define HONCH_MAX_AUTO_PROPERTIES HONCH_MAX_EVENT_PROPERTIES
#endif
#ifndef HONCH_AUTO_PROPERTY_BUFFER_COUNT
#define HONCH_AUTO_PROPERTY_BUFFER_COUNT 2u
#endif
#ifndef HONCH_WIRE_V2_MAX_FRAME_BYTES
#define HONCH_WIRE_V2_MAX_FRAME_BYTES 4096u
#endif
#define HONCH_DEFAULT_FLUSH_SCRATCH_MAX_EVENTS 4u
#ifndef HONCH_FLUSH_SCRATCH_MAX_EVENTS
#define HONCH_FLUSH_SCRATCH_MAX_EVENTS HONCH_DEFAULT_FLUSH_SCRATCH_MAX_EVENTS
#endif
#if HONCH_FLUSH_SCRATCH_MAX_EVENTS == 0u
#error "HONCH_FLUSH_SCRATCH_MAX_EVENTS must be greater than zero"
#endif
#if HONCH_FLUSH_SCRATCH_MAX_EVENTS > HONCH_MAX_BATCH_SIZE
#error "HONCH_FLUSH_SCRATCH_MAX_EVENTS cannot exceed HONCH_MAX_BATCH_SIZE"
#endif

/* Automatic $error log capture coalesces identical error lines in a small,
 * fixed, RAM-bounded table that is drained (enqueued) on flush/tick. */
#define HONCH_LOG_DEDUP_SLOTS 4u
#define HONCH_LOG_COMPONENT_STORE_BYTES 32u
#define HONCH_LOG_MESSAGE_STORE_BYTES 128u

/* Coredump upload reads the flash image one bounded chunk at a time; this caps
 * the per-step RAM cost (one chunk + its encoded frame), independent of image
 * size. The encoded frame adds a small header/varint/CRC overhead. */
#define HONCH_COREDUMP_CHUNK_BYTES 512u
#define HONCH_COREDUMP_FRAME_BYTES (HONCH_COREDUMP_CHUNK_BYTES + 32u)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct honch_log_error_slot {
    bool active;
    uint32_t hash;
    uint32_t count;
    char component[HONCH_LOG_COMPONENT_STORE_BYTES + 1u];
    char message[HONCH_LOG_MESSAGE_STORE_BYTES + 1u];
} honch_log_error_slot_t;

typedef struct honch_atomic_bool {
    bool value;
} honch_atomic_bool_t;

static inline void honch_atomic_bool_init(honch_atomic_bool_t *target, bool value)
{
    __atomic_store_n(&target->value, value, __ATOMIC_RELAXED);
}

static inline bool honch_atomic_bool_compare_exchange(
    honch_atomic_bool_t *target,
    bool *expected,
    bool desired)
{
    return __atomic_compare_exchange_n(
        &target->value,
        expected,
        desired,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);
}

static inline void honch_atomic_bool_store(honch_atomic_bool_t *target, bool value)
{
    __atomic_store_n(&target->value, value, __ATOMIC_RELEASE);
}

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
    honch_wire_v2_property_t properties[HONCH_MAX_EVENT_PROPERTIES];
    size_t property_count;
} honch_event_record_t;

typedef struct honch_wire_v2_encode_context {
    const char *device_id;
    const char *device_model;
    const char *firmware_version;
    const char *sdk_platform;
    const char *environment;
    const char *session_id;
    bool has_boot_epoch_ms;
    uint64_t boot_epoch_ms;
    uint64_t flush_uptime_ms;
} honch_wire_v2_encode_context_t;

typedef honch_status_t (*honch_wire_v2_event_provider_fn)(
    void *ctx,
    size_t index,
    honch_wire_v2_event_t *event);

struct honch_client {
    void *lifetime_mutex;
    void *state_mutex;
    honch_platform_ops_t platform_ops;
    const honch_platform_ops_t *platform;
    honch_state_storage_ops_t state_storage_ops;
    const honch_state_storage_ops_t *state_storage;
    honch_event_queue_ops_t event_queue_ops;
    const honch_event_queue_ops_t *event_queue;
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
    honch_wire_v2_property_t auto_property_buffers[HONCH_AUTO_PROPERTY_BUFFER_COUNT][HONCH_MAX_AUTO_PROPERTIES];
    honch_atomic_bool_t auto_property_buffer_in_use[HONCH_AUTO_PROPERTY_BUFFER_COUNT];
    honch_payload_t flush_events[HONCH_FLUSH_SCRATCH_MAX_EVENTS];
    bool flush_event_borrowed[HONCH_FLUSH_SCRATCH_MAX_EVENTS];
    uint64_t flush_sequences[HONCH_FLUSH_SCRATCH_MAX_EVENTS];
    honch_storage_event_t flush_storage_events[HONCH_FLUSH_SCRATCH_MAX_EVENTS];
    honch_event_record_t flush_parsed_record;
    uint8_t flush_message_buffer[HONCH_WIRE_V2_MAX_FRAME_BYTES];
    uint8_t flush_frame_buffer[HONCH_WIRE_V2_MAX_FRAME_BYTES];
    size_t pending_flush_message_size;
    size_t pending_flush_message_offset;
    size_t pending_flush_event_count;
    uint32_t pending_flush_message_id;
    char pending_flush_stream_id[9];
    bool pending_flush_active;
    bool configured_device_id;
    size_t batch_size;
    size_t max_queued_events;
    size_t max_event_bytes;
    unsigned int transport_timeout_ms;
    unsigned int flush_interval_seconds;
    unsigned int flush_min_interval_ms;
    size_t flush_event_threshold;
    size_t flush_max_batches;
    size_t shutdown_flush_max_batches;
    unsigned int flush_retry_initial_ms;
    unsigned int flush_retry_max_ms;
    uint32_t wire_v2_message_id_seed;
    char wire_v2_stream_id[9];
    honch_durability_mode_t durability_mode;
    uint64_t next_interval_flush_ms;
    uint64_t next_retry_flush_ms;
    uint64_t next_outbound_flush_ms;
    unsigned int current_retry_delay_ms;
    uint64_t active_storage_reader_sequence;
    bool scheduler_flush_requested;
    bool flush_in_progress;
    bool outbound_upload_attempted;
    bool uploads_paused;
    bool closing;
    size_t active_calls;
    int (*battery_callback)(void);
    int battery_low_threshold;
    honch_auto_properties_fn auto_properties_callback;
    void *auto_properties_userdata;
    honch_connectivity_fn connectivity_callback;
    void *connectivity_userdata;
    bool battery_low_emitted;
    uint64_t sequence;
    size_t queued_event_count;
    /* Automatic error/crash reporting state. */
    void (*crash_uploaded_callback)(void *userdata);
    void *crash_uploaded_userdata;
    bool crash_reported;        /* a $crash has been emitted this client lifetime (once-only) */
    bool crash_pending_ack;     /* a reported $crash is enqueued, awaiting delivery */
    bool crash_ack_due;         /* the $crash was delivered; fire the callback once, outside the lock */
    uint64_t crash_event_sequence; /* queue sequence of the enqueued $crash, for erase-after-ack */
    char coredump_crash_id[33]; /* links the $crash summary to its uploaded coredump blob */
    honch_log_error_slot_t log_error_slots[HONCH_LOG_DEDUP_SLOTS];
    uint32_t log_errors_dropped;
#if HONCH_ENABLE_CRASH_CAPTURE
    /* Raw coredump upload (streamed from flash, never buffered whole). */
    const honch_coredump_source_t *coredump_source;
    bool coredump_upload_active;  /* an upload is in progress */
    bool coredump_clear_due;      /* erase the source, fired outside the lock after final ack */
    size_t coredump_total;        /* image size captured at upload start */
    size_t coredump_offset;       /* next byte to send (advances only on ack) */
    uint16_t coredump_committed_crc; /* CRC of bytes [0, coredump_offset), advances only on ack */
    uint32_t coredump_message_id; /* wire message id for this upload */
    uint8_t coredump_chunk[HONCH_COREDUMP_CHUNK_BYTES];
    uint8_t coredump_frame[HONCH_COREDUMP_FRAME_BYTES];
#endif
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
void honch_core_wire_v2_context_from_client(
    honch_client_t *client,
    honch_wire_v2_encode_context_t *context);
uint64_t honch_core_normalize_wire_v2_timestamp(
    const honch_wire_v2_encode_context_t *context,
    uint64_t timestamp_ms);
honch_status_t honch_core_encode_single_wire_v2_event(
    const honch_wire_v2_encode_context_t *context,
    const honch_payload_t *event,
    uint8_t *buffer,
    size_t buffer_capacity,
    honch_payload_t *message);
honch_status_t honch_wire_v2_encode_event_batch_provider(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    honch_wire_v2_event_provider_fn provider,
    void *provider_ctx,
    size_t event_count,
    uint8_t *out,
    size_t out_size,
    size_t *written);
honch_status_t honch_wire_v2_measure_event_batch_provider(
    const honch_wire_v2_batch_context_t *context,
    uint64_t base_time_ms,
    honch_wire_v2_event_provider_fn provider,
    void *provider_ctx,
    size_t event_count,
    size_t *written);

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
/* fsync a directory so prior entry changes (unlink/rename) are durable. */
honch_status_t honch_fsync_directory(const char *directory);

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
honch_status_t honch_queue_flush_one_chunk_locked(honch_client_t *client, bool *progressed);
honch_status_t honch_queue_flush_limited_locked(honch_client_t *client, size_t max_batches);
honch_status_t honch_queue_flush_locked(honch_client_t *client);

honch_status_t honch_coredump_chunk(
    const honch_coredump_source_t *source,
    size_t offset,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *out_size);
#if HONCH_ENABLE_CRASH_CAPTURE
honch_status_t honch_coredump_upload_step_locked(honch_client_t *client, bool *progressed);
#endif

honch_status_t honch_client_enter(honch_client_t *client);
void honch_client_leave(honch_client_t *client);
honch_status_t honch_client_begin_shutdown(honch_client_t *client);
bool honch_client_lock_ops_valid(const honch_platform_ops_t *platform);
honch_status_t honch_client_lock_create(honch_client_t *client, void **mutex);
void honch_client_lock_destroy(honch_client_t *client, void *mutex);
honch_status_t honch_client_state_lock(honch_client_t *client);
void honch_client_state_unlock(honch_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
