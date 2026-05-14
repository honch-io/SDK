#include "honch/honch.h"
#define HONCH_BENCH_ALLOC_NO_WRAP
#include "honch_bench_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct bench_transport {
    long status;
    size_t calls;
    size_t bytes;
    size_t gzip_calls;
    size_t identity_calls;
    size_t max_body_bytes;
} bench_transport_t;

typedef struct queue_stats {
    size_t pending_files;
    size_t pending_bytes;
    size_t dead_files;
    size_t dead_bytes;
} queue_stats_t;

typedef struct bench_result {
    const char *name;
    const char *phase;
    size_t iterations;
    size_t events_per_iteration;
    unsigned long long total_us;
    unsigned long long min_us;
    unsigned long long p50_us;
    unsigned long long p95_us;
    unsigned long long p99_us;
    unsigned long long max_us;
    long peak_rss_kb;
    honch_bench_alloc_stats_t alloc;
    bench_transport_t transport;
    queue_stats_t queue;
    honch_status_t status;
} bench_result_t;

static unsigned long long now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((unsigned long long)ts.tv_sec * 1000000ull) + ((unsigned long long)ts.tv_nsec / 1000ull);
}

static long peak_rss_kb(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

static int compare_ull(const void *left, const void *right)
{
    unsigned long long a = *(const unsigned long long *)left;
    unsigned long long b = *(const unsigned long long *)right;
    return (a > b) - (a < b);
}

static unsigned long long percentile(unsigned long long *samples, size_t count, unsigned int pct)
{
    if (count == 0u) {
        return 0u;
    }
    qsort(samples, count, sizeof(samples[0]), compare_ull);
    size_t index = ((count - 1u) * (size_t)pct + 99u) / 100u;
    if (index >= count) {
        index = count - 1u;
    }
    return samples[index];
}

static int remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (remove_tree(child) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (unlink(child) != 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return rmdir(path);
}

static int make_temp_dir(char *path, size_t size)
{
    snprintf(path, size, "/tmp/honch-bench-%ld-XXXXXX", (long)getpid());
    return mkdtemp(path) == NULL ? -1 : 0;
}

static void scan_cbor_files(const char *directory, size_t *files, size_t *bytes)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 5u || strcmp(entry->d_name + len - 5u, ".cbor") != 0) {
            continue;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            (*files)++;
            if (st.st_size > 0) {
                *bytes += (size_t)st.st_size;
            }
        }
    }

    closedir(dir);
}

static queue_stats_t collect_queue_stats(const char *queue_dir)
{
    queue_stats_t stats = {0};
    char pending[PATH_MAX];
    char dead[PATH_MAX];
    snprintf(pending, sizeof(pending), "%s/pending", queue_dir);
    snprintf(dead, sizeof(dead), "%s/dead", queue_dir);
    scan_cbor_files(pending, &stats.pending_files, &stats.pending_bytes);
    scan_cbor_files(dead, &stats.dead_files, &stats.dead_bytes);
    return stats;
}

static honch_status_t fake_transport(
    const char *url,
    const char *api_key,
    const unsigned char *body,
    size_t body_size,
    const char *content_encoding,
    void *userdata,
    long *http_status)
{
    (void)url;
    (void)api_key;
    bench_transport_t *transport = (bench_transport_t *)userdata;
    transport->calls++;
    transport->bytes += body_size;
    if (body_size > transport->max_body_bytes) {
        transport->max_body_bytes = body_size;
    }
    if (content_encoding != NULL && strcmp(content_encoding, "gzip") == 0) {
        transport->gzip_calls++;
    } else {
        transport->identity_calls++;
    }
    if (body == NULL && body_size != 0u) {
        return HONCH_ERROR_TRANSPORT;
    }
    *http_status = transport->status;
    return HONCH_OK;
}

static honch_config_t bench_config(const char *queue_dir)
{
    honch_config_t config = {
        .api_key = "bench-key",
        .endpoint_url = "http://127.0.0.1:9",
        .device_id = "bench-device",
        .device_model = "bench-model",
        .firmware_version = "1.0.0",
        .environment = "bench",
        .queue_directory = queue_dir,
        .batch_size = 50u,
        .max_queued_events = 10000u,
        .max_event_bytes = 65536u,
        .transport_timeout_ms = 1000u,
        .flush_interval_seconds = 60u,
        .flush_event_threshold = 30u,
        .flush_retry_initial_ms = 1000u,
        .flush_retry_max_ms = 300000u,
        .disable_gzip = 1,
        .gzip_min_bytes = 1024u,
        .disable_background_flush = 1,
        .battery_callback = NULL,
        .battery_low_threshold = 15
    };
    return config;
}

static void result_from_samples(
    bench_result_t *result,
    const char *name,
    const char *phase,
    unsigned long long *samples,
    size_t iterations,
    size_t events_per_iteration,
    bench_transport_t transport,
    queue_stats_t queue,
    honch_status_t status)
{
    memset(result, 0, sizeof(*result));
    result->name = name;
    result->phase = phase;
    result->iterations = iterations;
    result->events_per_iteration = events_per_iteration;
    result->min_us = ULLONG_MAX;
    for (size_t i = 0u; i < iterations; i++) {
        result->total_us += samples[i];
        if (samples[i] < result->min_us) {
            result->min_us = samples[i];
        }
        if (samples[i] > result->max_us) {
            result->max_us = samples[i];
        }
    }
    result->p50_us = percentile(samples, iterations, 50u);
    result->p95_us = percentile(samples, iterations, 95u);
    result->p99_us = percentile(samples, iterations, 99u);
    result->peak_rss_kb = peak_rss_kb();
    honch_bench_alloc_get(&result->alloc);
    result->transport = transport;
    result->queue = queue;
    result->status = status;
}

static void print_header(void)
{
    printf("name,phase,iterations,events_per_iteration,total_us,mean_us,min_us,p50_us,p95_us,p99_us,max_us,peak_rss_kb,sdk_current_bytes,sdk_peak_bytes,sdk_total_allocated_bytes,sdk_total_freed_bytes,sdk_malloc_calls,sdk_calloc_calls,sdk_realloc_calls,sdk_free_calls,sdk_failed_allocations,sdk_live_allocations,sdk_peak_live_allocations,transport_calls,transport_bytes,transport_max_body_bytes,transport_identity_calls,transport_gzip_calls,queue_pending_files,queue_pending_bytes,queue_dead_files,queue_dead_bytes,status\n");
}

static void print_result(const bench_result_t *result)
{
    double mean = result->iterations == 0u ? 0.0 : (double)result->total_us / (double)result->iterations;
    printf("%s,%s,%zu,%zu,%llu,%.2f,%llu,%llu,%llu,%llu,%llu,%ld,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%s\n",
           result->name,
           result->phase,
           result->iterations,
           result->events_per_iteration,
           result->total_us,
           mean,
           result->min_us == ULLONG_MAX ? 0u : result->min_us,
           result->p50_us,
           result->p95_us,
           result->p99_us,
           result->max_us,
           result->peak_rss_kb,
           result->alloc.current_bytes,
           result->alloc.peak_bytes,
           result->alloc.total_allocated_bytes,
           result->alloc.total_freed_bytes,
           result->alloc.malloc_calls,
           result->alloc.calloc_calls,
           result->alloc.realloc_calls,
           result->alloc.free_calls,
           result->alloc.failed_allocations,
           result->alloc.live_allocations,
           result->alloc.peak_live_allocations,
           result->transport.calls,
           result->transport.bytes,
           result->transport.max_body_bytes,
           result->transport.identity_calls,
           result->transport.gzip_calls,
           result->queue.pending_files,
           result->queue.pending_bytes,
           result->queue.dead_files,
           result->queue.dead_bytes,
           honch_status_string(result->status));
}

static int run_init_shutdown(void)
{
    const size_t iterations = 200u;
    unsigned long long *samples = (unsigned long long *)calloc(iterations, sizeof(*samples));
    if (samples == NULL) {
        return 1;
    }

    honch_bench_alloc_reset();
    bench_transport_t transport = {.status = 202L};
    honch_test_set_transport(fake_transport, &transport);
    honch_status_t status = HONCH_OK;

    for (size_t i = 0u; i < iterations; i++) {
        char queue_dir[PATH_MAX];
        if (make_temp_dir(queue_dir, sizeof(queue_dir)) != 0) {
            free(samples);
            return 1;
        }

        honch_config_t config = bench_config(queue_dir);
        honch_client_t *client = NULL;
        unsigned long long start = now_us();
        status = honch_init(&client, &config);
        if (status == HONCH_OK) {
            status = honch_shutdown(client);
        }
        samples[i] = now_us() - start;
        remove_tree(queue_dir);
        if (status != HONCH_OK) {
            break;
        }
    }

    bench_result_t result;
    result_from_samples(&result, "init_shutdown", "full_lifecycle", samples, iterations, 0u, transport, (queue_stats_t){0}, status);
    print_result(&result);
    honch_test_set_transport(NULL, NULL);
    free(samples);
    return status == HONCH_OK ? 0 : 1;
}

static int run_track_scenario(const char *name, const char *properties, size_t iterations)
{
    unsigned long long *samples = (unsigned long long *)calloc(iterations, sizeof(*samples));
    if (samples == NULL) {
        return 1;
    }

    char queue_dir[PATH_MAX];
    if (make_temp_dir(queue_dir, sizeof(queue_dir)) != 0) {
        free(samples);
        return 1;
    }

    honch_bench_alloc_reset();
    bench_transport_t transport = {.status = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_config_t config = bench_config(queue_dir);
    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    for (size_t i = 0u; status == HONCH_OK && i < iterations; i++) {
        unsigned long long start = now_us();
        status = honch_track(client, "bench_event", properties);
        samples[i] = now_us() - start;
    }

    queue_stats_t queue = collect_queue_stats(queue_dir);
    bench_result_t result;
    result_from_samples(&result, name, "track", samples, iterations, 1u, transport, queue, status);
    print_result(&result);

    if (client != NULL) {
        honch_status_t shutdown_status = honch_shutdown(client);
        if (status == HONCH_OK) {
            status = shutdown_status;
        }
    }
    honch_test_set_transport(NULL, NULL);
    remove_tree(queue_dir);
    free(samples);
    return status == HONCH_OK ? 0 : 1;
}

static int run_flush_scenario(const char *name, size_t events, int gzip_enabled, long http_status)
{
    unsigned long long sample = 0u;
    char queue_dir[PATH_MAX];
    if (make_temp_dir(queue_dir, sizeof(queue_dir)) != 0) {
        return 1;
    }

    honch_bench_alloc_reset();
    bench_transport_t transport = {.status = http_status};
    honch_test_set_transport(fake_transport, &transport);

    honch_config_t config = bench_config(queue_dir);
    config.disable_gzip = gzip_enabled ? 0 : 1;
    config.gzip_min_bytes = gzip_enabled ? 0u : 1024u;

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config);
    for (size_t i = 0u; status == HONCH_OK && i < events; i++) {
        status = honch_track(client, "bench_flush_event", "{\"mode\":\"hdr\",\"frames\":120,\"ok\":true}");
    }

    transport = (bench_transport_t){.status = http_status};
    honch_test_set_transport(fake_transport, &transport);

    if (status == HONCH_OK) {
        unsigned long long start = now_us();
        status = honch_flush(client);
        sample = now_us() - start;
    }

    queue_stats_t queue = collect_queue_stats(queue_dir);
    bench_result_t result;
    result_from_samples(&result, name, "flush", &sample, 1u, events, transport, queue, status);
    print_result(&result);

    if (client != NULL) {
        config.disable_gzip = 1;
        honch_test_set_transport(fake_transport, &(bench_transport_t){.status = 202L});
        (void)honch_shutdown(client);
    }
    honch_test_set_transport(NULL, NULL);
    remove_tree(queue_dir);
    return (http_status >= 200L && http_status < 300L) ? (status == HONCH_OK ? 0 : 1) : 0;
}

int main(void)
{
    print_header();

    if (run_init_shutdown() != 0) {
        return 1;
    }
    if (run_track_scenario("track_empty_properties", "{}", 1000u) != 0) {
        return 1;
    }
    if (run_track_scenario("track_small_properties", "{\"button\":1,\"mode\":\"single\"}", 1000u) != 0) {
        return 1;
    }
    if (run_track_scenario("track_nested_properties", "{\"mode\":\"hdr\",\"nested\":{\"iso\":800,\"stabilized\":true},\"tags\":[\"field\",\"beta\"],\"ratio\":1.5}", 1000u) != 0) {
        return 1;
    }
    if (run_track_scenario("track_1kb_properties", "{\"blob\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", 500u) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_1_raw_success", 1u, 0, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_raw_success", 50u, 0, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_200_raw_success", 200u, 0, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_gzip_success", 50u, 1, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_retry_500", 50u, 0, 500L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_rejected_400", 50u, 0, 400L) != 0) {
        return 1;
    }

    return 0;
}
