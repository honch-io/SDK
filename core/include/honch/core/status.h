#ifndef HONCH_CORE_STATUS_H
#define HONCH_CORE_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum honch_status {
    HONCH_OK = 0,
    HONCH_ERROR_INVALID_ARGUMENT = 1,
    HONCH_ERROR_OUT_OF_MEMORY = 2,
    HONCH_ERROR_IO = 3,
    HONCH_ERROR_TRANSPORT = 4,
    HONCH_ERROR_RATE_LIMITED = 5,
    HONCH_ERROR_SERVER = 6,
    HONCH_ERROR_REJECTED = 7,
    HONCH_ERROR_NOT_INITIALIZED = 8,
    HONCH_ERROR_ALREADY_INITIALIZED = 9,
    HONCH_ERROR_QUEUE_FULL = 10,
    HONCH_ERROR_TIMEOUT = 11,
    HONCH_ERROR_INTERNAL = 12
} honch_status_t;

#ifdef __cplusplus
}
#endif

#endif
