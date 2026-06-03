#ifndef HONCH_CORE_PLATFORM_H
#define HONCH_CORE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "honch/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_log_level {
    HONCH_LOG_DEBUG,
    HONCH_LOG_INFO,
    HONCH_LOG_WARN,
    HONCH_LOG_ERROR
} honch_log_level_t;

typedef struct honch_platform_ops {
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*uptime_ms)(void *ctx);
    honch_status_t (*random_bytes)(void *ctx, uint8_t *buffer, size_t buffer_size);
    void (*log)(void *ctx, honch_log_level_t level, const char *message);
    honch_status_t (*mutex_create)(void *ctx, void **mutex);
    void (*mutex_destroy)(void *ctx, void *mutex);
    honch_status_t (*mutex_lock)(void *ctx, void *mutex);
    void (*mutex_unlock)(void *ctx, void *mutex);
    uint8_t requires_mutex;
    void *ctx;
} honch_platform_ops_t;

#ifdef __cplusplus
}
#endif

#endif
