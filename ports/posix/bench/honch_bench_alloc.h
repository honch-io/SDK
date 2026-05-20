#ifndef HONCH_BENCH_ALLOC_H
#define HONCH_BENCH_ALLOC_H

#include <stddef.h>
#include <stdlib.h>

typedef struct honch_bench_alloc_stats {
    size_t current_bytes;
    size_t peak_bytes;
    size_t total_allocated_bytes;
    size_t total_freed_bytes;
    size_t malloc_calls;
    size_t calloc_calls;
    size_t realloc_calls;
    size_t free_calls;
    size_t failed_allocations;
    size_t live_allocations;
    size_t peak_live_allocations;
} honch_bench_alloc_stats_t;

void *honch_bench_malloc(size_t size);
void *honch_bench_calloc(size_t count, size_t size);
void *honch_bench_realloc(void *ptr, size_t size);
void honch_bench_free(void *ptr);
void honch_bench_alloc_reset(void);
void honch_bench_alloc_get(honch_bench_alloc_stats_t *out);

#ifndef HONCH_BENCH_ALLOC_NO_WRAP
  #define malloc(size) honch_bench_malloc(size)
  #define calloc(count, size) honch_bench_calloc((count), (size))
  #define realloc(ptr, size) honch_bench_realloc((ptr), (size))
  #define free(ptr) honch_bench_free(ptr)
#endif

#endif
