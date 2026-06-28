#ifndef HONCH_CORE_COREDUMP_H
#define HONCH_CORE_COREDUMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A port-supplied view over a crash coredump image that is held on the device
 * (e.g. the ESP-IDF coredump flash partition). The core streams the image off in
 * bounded chunks via this interface so the multi-KB blob NEVER has to be loaded
 * into RAM or the event queue. The image is read-only and is expected to remain
 * stable across the upload (it was written by the platform at crash time and is
 * only cleared once delivery is acknowledged).
 */
typedef struct honch_coredump_source {
    /* Total image size in bytes, or 0 when no coredump is available. */
    size_t (*size)(void *ctx);
    /* Read exactly `len` bytes at byte `offset` into `out`. Returns the number of
     * bytes read (== len on success) or a negative value on error. Implemented by
     * the port reading from its backing store (flash); must not buffer the whole
     * image. */
    int (*read)(void *ctx, size_t offset, uint8_t *out, size_t len);
    /* Clear the on-device image after the blob has been fully delivered and
     * acknowledged (erase-after-ack), so it is not re-uploaded on the next boot.
     * Optional; may be NULL when the port clears the source by other means. */
    void (*clear)(void *ctx);
    void *ctx;
} honch_coredump_source_t;

#ifdef __cplusplus
}
#endif

#endif
