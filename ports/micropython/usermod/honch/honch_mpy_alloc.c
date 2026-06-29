// MicroPython implementation of the core's optional allocator hook
// (HONCH_USE_MP_ALLOC, declared in core/src/honch_internal.h).
//
// Routes honch core allocations to the MicroPython GC heap instead of the tiny
// rp2 system/newlib heap. On a 264KB Pico W the system heap is largely consumed
// by cyw43/lwIP/mbedtls once Wi-Fi is up, so honch_core_init's ~18KB calloc
// fault/hung there; the GC heap has hundreds of KB free. gc_alloc/gc_free take
// no size-on-free, sidestepping the m_free MICROPY_MALLOC_USES_ALLOCATED_SIZE
// variance, and return NULL on failure (no nlr raise) -- matching libc malloc
// semantics the core already handles.
//
// GC reachability: the honch_client_t is held by the MicroPython client object
// (mp_obj_malloc), so it stays reachable and is conservatively scanned, keeping
// its internal allocations (strings, queue, buffers) alive. The GC is
// non-moving, so the raw pointers the core holds remain valid.

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "py/runtime.h"
#include "py/gc.h"

void *honch_port_malloc(size_t size) {
    if (size == 0u) {
        size = 1u;
    }
    return gc_alloc(size, 0);
}

void *honch_port_calloc(size_t count, size_t size) {
    size_t total;
    if (count != 0u && size > (size_t)-1 / count) {
        return NULL;  // multiplication would overflow
    }
    total = count * size;
    if (total == 0u) {
        total = 1u;
    }
    void *ptr = gc_alloc(total, 0);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *honch_port_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return honch_port_malloc(size);
    }
    if (size == 0u) {
        size = 1u;
    }
    return gc_realloc(ptr, size, true /* allow_move */);
}

void honch_port_free(void *ptr) {
    if (ptr != NULL) {
        gc_free(ptr);
    }
}
