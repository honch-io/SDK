#ifndef HONCH_MICROPYTHON_H
#define HONCH_MICROPYTHON_H

#define HONCH_CORE_NO_SHORT_STATUS_NAMES

#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"
#include "py/builtin.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "honch/core/honch.h"
#include "honch/core/platform.h"
#include "honch/core/ram_queue.h"
#include "honch/core/storage.h"
#include "honch/core/transport.h"

typedef struct honch_micropython_platform {
    mp_obj_t os_module;
    mp_obj_t time_module;
    mp_obj_t random_module;
} honch_micropython_platform_t;

typedef struct honch_micropython_storage {
    honch_ram_queue_t ram_queue;
} honch_micropython_storage_t;

typedef struct honch_micropython_transport {
    mp_obj_t requests_module;
    unsigned int timeout_ms;
} honch_micropython_transport_t;

honch_status_t honch_micropython_platform_ops_init(
    honch_platform_ops_t *ops,
    honch_micropython_platform_t *ctx);

honch_status_t honch_micropython_storage_ops_init(
    honch_event_queue_ops_t *ops,
    honch_micropython_storage_t *ctx,
    uint8_t *buffer,
    size_t buffer_size);

void honch_micropython_storage_ops_deinit(honch_micropython_storage_t *ctx);

honch_status_t honch_micropython_transport_ops_init(
    honch_transport_ops_t *ops,
    honch_micropython_transport_t *ctx,
    unsigned int timeout_ms);

void honch_micropython_raise_status(honch_status_t status);
char *honch_micropython_strdup(const char *value);
honch_status_t honch_micropython_join_path(char **out, const char *left, const char *right);
honch_status_t honch_micropython_call_noarg_attr(mp_obj_t object, qstr attr);

#if HONCH_MICROPYTHON_DEBUG_INIT
#define HONCH_MP_DEBUG_INIT(message) do { \
    mp_hal_stdout_tx_strn("honch:init:" message "\n", sizeof("honch:init:" message "\n") - 1); \
} while (0)
#else
#define HONCH_MP_DEBUG_INIT(message) ((void)0)
#endif

#endif
