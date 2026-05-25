#define HONCH_BENCH_ALLOC_NO_WRAP
#include "honch_bench_alloc.h"

#undef malloc
#undef calloc
#undef realloc
#undef free

#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef struct allocation_header {
    size_t size;
} allocation_header_t;

static pthread_mutex_t s_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static honch_bench_alloc_stats_t s_stats;

static void add_live_locked(size_t size)
{
    s_stats.current_bytes += size;
    s_stats.total_allocated_bytes += size;
    s_stats.live_allocations++;
    if (s_stats.current_bytes > s_stats.peak_bytes) {
        s_stats.peak_bytes = s_stats.current_bytes;
    }
    if (s_stats.live_allocations > s_stats.peak_live_allocations) {
        s_stats.peak_live_allocations = s_stats.live_allocations;
    }
}

static void remove_live_locked(size_t size)
{
    if (s_stats.current_bytes >= size) {
        s_stats.current_bytes -= size;
    } else {
        s_stats.current_bytes = 0u;
    }
    s_stats.total_freed_bytes += size;
    if (s_stats.live_allocations > 0u) {
        s_stats.live_allocations--;
    }
}

void *honch_bench_malloc(size_t size)
{
    size_t total = size + sizeof(allocation_header_t);
    if (total < size) {
        pthread_mutex_lock(&s_alloc_mutex);
        s_stats.malloc_calls++;
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }

    allocation_header_t *header = (allocation_header_t *)malloc(total);
    pthread_mutex_lock(&s_alloc_mutex);
    s_stats.malloc_calls++;
    if (header == NULL) {
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }
    header->size = size;
    add_live_locked(size);
    pthread_mutex_unlock(&s_alloc_mutex);
    return (void *)(header + 1);
}

void *honch_bench_calloc(size_t count, size_t size)
{
    if (size != 0u && count > SIZE_MAX / size) {
        pthread_mutex_lock(&s_alloc_mutex);
        s_stats.calloc_calls++;
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }

    size_t bytes = count * size;
    void *ptr = honch_bench_malloc(bytes);
    pthread_mutex_lock(&s_alloc_mutex);
    s_stats.calloc_calls++;
    if (ptr == NULL) {
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }
    pthread_mutex_unlock(&s_alloc_mutex);
    memset(ptr, 0, bytes);
    return ptr;
}

void *honch_bench_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        void *next = honch_bench_malloc(size);
        pthread_mutex_lock(&s_alloc_mutex);
        s_stats.realloc_calls++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return next;
    }

    if (size == 0u) {
        honch_bench_free(ptr);
        pthread_mutex_lock(&s_alloc_mutex);
        s_stats.realloc_calls++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }

    allocation_header_t *old_header = ((allocation_header_t *)ptr) - 1;
    size_t old_size = old_header->size;
    size_t total = size + sizeof(allocation_header_t);
    if (total < size) {
        pthread_mutex_lock(&s_alloc_mutex);
        s_stats.realloc_calls++;
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }

    allocation_header_t *new_header = (allocation_header_t *)realloc(old_header, total);
    pthread_mutex_lock(&s_alloc_mutex);
    s_stats.realloc_calls++;
    if (new_header == NULL) {
        s_stats.failed_allocations++;
        pthread_mutex_unlock(&s_alloc_mutex);
        return NULL;
    }

    new_header->size = size;
    remove_live_locked(old_size);
    add_live_locked(size);
    pthread_mutex_unlock(&s_alloc_mutex);
    return (void *)(new_header + 1);
}

void honch_bench_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    allocation_header_t *header = ((allocation_header_t *)ptr) - 1;
    pthread_mutex_lock(&s_alloc_mutex);
    s_stats.free_calls++;
    remove_live_locked(header->size);
    pthread_mutex_unlock(&s_alloc_mutex);
    free(header);
}

void honch_bench_alloc_reset(void)
{
    pthread_mutex_lock(&s_alloc_mutex);
    memset(&s_stats, 0, sizeof(s_stats));
    pthread_mutex_unlock(&s_alloc_mutex);
}

void honch_bench_alloc_get(honch_bench_alloc_stats_t *out)
{
    pthread_mutex_lock(&s_alloc_mutex);
    *out = s_stats;
    pthread_mutex_unlock(&s_alloc_mutex);
}
