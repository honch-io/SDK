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

#define RATE_SWEEP_MAX_RATES 16u
#define RATE_SWEEP_DEFAULT_DURATION_SECONDS 20u
#define RATE_SWEEP_DEFAULT_WARMUP_SECONDS 3u
#define RATE_SWEEP_DEFAULT_WINDOW_SECONDS 1u
#define RATE_SWEEP_DEFAULT_PAYLOAD_BYTES 64u
#define RATE_SWEEP_DEFAULT_FLUSH_EVERY 100u

typedef struct bench_transport {
    long status;
    size_t calls;
    size_t bytes;
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

typedef struct rate_sweep_config {
    unsigned int rates[RATE_SWEEP_MAX_RATES];
    size_t rate_count;
    unsigned int duration_seconds;
    unsigned int warmup_seconds;
    unsigned int window_seconds;
    size_t payload_bytes;
    size_t flush_every;
} rate_sweep_config_t;

typedef struct rate_window_result {
    unsigned int rate;
    unsigned int window;
    double actual_eps;
    size_t events_attempted;
    size_t events_accepted;
    size_t events_failed;
    unsigned long long track_avg_us;
    unsigned long long track_p50_us;
    unsigned long long track_p95_us;
    unsigned long long track_p99_us;
    unsigned long long track_max_us;
    size_t flush_count;
    size_t flush_failures;
    unsigned long long flush_avg_us;
    unsigned long long flush_p95_us;
    double cpu_percent;
    size_t queued_estimate;
    queue_stats_t queue;
    long rss_kb;
    honch_bench_alloc_stats_t alloc;
    bench_transport_t transport_delta;
} rate_window_result_t;

static unsigned long long now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((unsigned long long)ts.tv_sec * 1000000ull) + ((unsigned long long)ts.tv_nsec / 1000ull);
}

static unsigned long long rusage_cpu_us(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0u;
    }
    unsigned long long user =
        ((unsigned long long)usage.ru_utime.tv_sec * 1000000ull) +
        (unsigned long long)usage.ru_utime.tv_usec;
    unsigned long long sys =
        ((unsigned long long)usage.ru_stime.tv_sec * 1000000ull) +
        (unsigned long long)usage.ru_stime.tv_usec;
    return user + sys;
}

static void sleep_until_us(unsigned long long deadline_us)
{
    for (;;) {
        unsigned long long now = now_us();
        if (now >= deadline_us) {
            return;
        }
        unsigned long long remaining = deadline_us - now;
        struct timespec ts = {
            .tv_sec = (time_t)(remaining / 1000000ull),
            .tv_nsec = (long)((remaining % 1000000ull) * 1000ull),
        };
        if (nanosleep(&ts, NULL) == 0 || errno != EINTR) {
            return;
        }
    }
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

static unsigned long long sum_samples(const unsigned long long *samples, size_t count)
{
    unsigned long long total = 0u;
    for (size_t i = 0u; i < count; i++) {
        total += samples[i];
    }
    return total;
}

static unsigned long long max_sample(const unsigned long long *samples, size_t count)
{
    unsigned long long max = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (samples[i] > max) {
            max = samples[i];
        }
    }
    return max;
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
    const char *stream_id,
    const unsigned char *body,
    size_t body_size,
    const char *content_encoding,
    void *userdata,
    long *http_status)
{
    (void)url;
    (void)api_key;
    (void)stream_id;
    (void)content_encoding;
    bench_transport_t *transport = (bench_transport_t *)userdata;
    transport->calls++;
    transport->bytes += body_size;
    if (body_size > transport->max_body_bytes) {
        transport->max_body_bytes = body_size;
    }
    if (body == NULL && body_size != 0u) {
        return HONCH_ERROR_TRANSPORT;
    }
    if (transport->status == 202L && body_size > 0u && body != NULL) {
        *http_status = (body[0] & 0x40u) != 0u ? 202L : 204L;
    } else {
        *http_status = transport->status;
    }
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
        .disable_background_flush = 1,
        .battery_callback = NULL,
        .battery_low_threshold = 15
    };
    return config;
}

static void fill_rate_payload(char *payload, size_t payload_size, size_t payload_bytes)
{
    static const char source[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (payload_size == 0u) {
        return;
    }
    size_t want = payload_bytes;
    if (want >= payload_size) {
        want = payload_size - 1u;
    }
    for (size_t i = 0u; i < want; i++) {
        payload[i] = source[i % (sizeof(source) - 1u)];
    }
    payload[want] = '\0';
}

static int parse_unsigned(const char *value, unsigned int *out)
{
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed > UINT_MAX) {
        return -1;
    }
    *out = (unsigned int)parsed;
    return 0;
}

static int parse_size_arg(const char *value, size_t *out)
{
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed > (unsigned long long)SIZE_MAX) {
        return -1;
    }
    *out = (size_t)parsed;
    return 0;
}

static int parse_rates(const char *value, rate_sweep_config_t *config)
{
    char copy[128];
    int written = snprintf(copy, sizeof(copy), "%s", value);
    if (written < 0 || written >= (int)sizeof(copy)) {
        return -1;
    }

    config->rate_count = 0u;
    char *cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        if (config->rate_count >= RATE_SWEEP_MAX_RATES) {
            return -1;
        }
        unsigned int rate = 0u;
        if (parse_unsigned(cursor, &rate) != 0 || rate == 0u) {
            return -1;
        }
        config->rates[config->rate_count++] = rate;
        cursor = comma == NULL ? NULL : comma + 1;
    }

    return config->rate_count == 0u ? -1 : 0;
}

static rate_sweep_config_t default_rate_sweep_config(void)
{
    rate_sweep_config_t config = {
        .rates = {100u, 500u, 1000u, 3000u},
        .rate_count = 4u,
        .duration_seconds = RATE_SWEEP_DEFAULT_DURATION_SECONDS,
        .warmup_seconds = RATE_SWEEP_DEFAULT_WARMUP_SECONDS,
        .window_seconds = RATE_SWEEP_DEFAULT_WINDOW_SECONDS,
        .payload_bytes = RATE_SWEEP_DEFAULT_PAYLOAD_BYTES,
        .flush_every = RATE_SWEEP_DEFAULT_FLUSH_EVERY,
    };
    return config;
}

static void print_rate_sweep_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--rate-sweep] [--rates 100,500,1000,3000] "
            "[--duration seconds] [--warmup seconds] [--window seconds] "
            "[--payload-bytes bytes] [--flush-every events]\n",
            argv0);
}

static int parse_rate_sweep_args(int argc, char **argv, rate_sweep_config_t *config, int *rate_sweep)
{
    *config = default_rate_sweep_config();
    *rate_sweep = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rate-sweep") == 0) {
            *rate_sweep = 1;
        } else if (strcmp(argv[i], "--rates") == 0 && i + 1 < argc) {
            if (parse_rates(argv[++i], config) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            if (parse_unsigned(argv[++i], &config->duration_seconds) != 0 ||
                config->duration_seconds == 0u) {
                return -1;
            }
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            if (parse_unsigned(argv[++i], &config->warmup_seconds) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            if (parse_unsigned(argv[++i], &config->window_seconds) != 0 ||
                config->window_seconds == 0u) {
                return -1;
            }
        } else if (strcmp(argv[i], "--payload-bytes") == 0 && i + 1 < argc) {
            if (parse_size_arg(argv[++i], &config->payload_bytes) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--flush-every") == 0 && i + 1 < argc) {
            if (parse_size_arg(argv[++i], &config->flush_every) != 0 ||
                config->flush_every == 0u) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (*rate_sweep == 0 && argc > 1) {
        return -1;
    }

    return 0;
}

static int rate_window_record_track(
    unsigned long long *samples,
    size_t sample_capacity,
    size_t *count,
    unsigned long long elapsed_us)
{
    if (*count >= sample_capacity) {
        return -1;
    }
    samples[*count] = elapsed_us;
    (*count)++;
    return 0;
}

static void print_rate_window(const rate_window_result_t *result)
{
    printf("BENCH_WINDOW runtime=c-core-posix mode=sdk-only rate_target_eps=%u "
           "window=%u actual_eps=%.2f events_attempted=%zu events_accepted=%zu "
           "events_failed=%zu track_avg_us=%llu track_p50_us=%llu "
           "track_p95_us=%llu track_p99_us=%llu track_max_us=%llu "
           "flush_count=%zu flush_failures=%zu flush_avg_us=%llu "
           "flush_p95_us=%llu cpu_percent=%.2f queued_estimate=%zu "
           "rss_kb=%ld sdk_current_bytes=%zu sdk_peak_bytes=%zu "
           "transport_calls=%zu transport_bytes=%zu queue_pending_files=%zu "
           "queue_pending_bytes=%zu queue_dead_files=%zu queue_dead_bytes=%zu\n",
           result->rate,
           result->window,
           result->actual_eps,
           result->events_attempted,
           result->events_accepted,
           result->events_failed,
           result->track_avg_us,
           result->track_p50_us,
           result->track_p95_us,
           result->track_p99_us,
           result->track_max_us,
           result->flush_count,
           result->flush_failures,
           result->flush_avg_us,
           result->flush_p95_us,
           result->cpu_percent,
           result->queued_estimate,
           result->rss_kb,
           result->alloc.current_bytes,
           result->alloc.peak_bytes,
           result->transport_delta.calls,
           result->transport_delta.bytes,
           result->queue.pending_files,
           result->queue.pending_bytes,
           result->queue.dead_files,
           result->queue.dead_bytes);
}

static int run_rate_window(
    honch_client_t *client,
    const rate_sweep_config_t *config,
    unsigned int rate,
    unsigned int window,
    char *properties,
    size_t properties_size,
    const char *payload,
    bench_transport_t *transport,
    const char *queue_dir,
    size_t *queued_estimate,
    size_t *global_sequence)
{
    size_t sample_capacity = ((size_t)rate * (size_t)config->window_seconds) + 1024u;
    size_t flush_capacity = (sample_capacity / config->flush_every) + 32u;
    unsigned long long *track_samples =
        (unsigned long long *)calloc(sample_capacity, sizeof(*track_samples));
    unsigned long long *flush_samples =
        (unsigned long long *)calloc(flush_capacity, sizeof(*flush_samples));
    if (track_samples == NULL || flush_samples == NULL) {
        free(track_samples);
        free(flush_samples);
        return 1;
    }

    bench_transport_t transport_before = *transport;
    unsigned long long cpu_start = rusage_cpu_us();
    unsigned long long window_start = now_us();
    unsigned long long window_end = window_start + ((unsigned long long)config->window_seconds * 1000000ull);
    size_t attempted = 0u;
    size_t accepted = 0u;
    size_t failed = 0u;
    size_t track_count = 0u;
    size_t flush_count = 0u;
    size_t flush_failures = 0u;
    size_t accepted_since_flush = 0u;
    honch_status_t status = HONCH_OK;

    while (now_us() < window_end) {
        unsigned long long deadline =
            window_start + (((unsigned long long)attempted * 1000000ull) / (unsigned long long)rate);
        sleep_until_us(deadline);

        snprintf(properties,
                 properties_size,
                 "{\"seq\":%zu,\"target_eps\":%u,\"window\":%u,\"sample\":%u,\"payload\":\"%s\"}",
                 *global_sequence,
                 rate,
                 window,
                 (unsigned int)((*global_sequence * 1103515245u + 12345u) & 0xffffu),
                 payload);

        unsigned long long start = now_us();
        status = honch_track(client, "bench_rate_event", properties);
        unsigned long long elapsed = now_us() - start;
        if (rate_window_record_track(track_samples, sample_capacity, &track_count, elapsed) != 0) {
            failed++;
            break;
        }

        attempted++;
        (*global_sequence)++;
        if (status == HONCH_OK) {
            accepted++;
            (*queued_estimate)++;
            accepted_since_flush++;
        } else {
            failed++;
        }

        if (accepted_since_flush >= config->flush_every) {
            start = now_us();
            honch_status_t flush_status = honch_flush(client);
            elapsed = now_us() - start;
            if (flush_count < flush_capacity) {
                flush_samples[flush_count++] = elapsed;
            }
            if (flush_status == HONCH_OK) {
                *queued_estimate = 0u;
                accepted_since_flush = 0u;
            } else {
                flush_failures++;
            }
        }
    }

    unsigned long long wall_us = now_us() - window_start;
    unsigned long long cpu_elapsed = rusage_cpu_us() - cpu_start;
    queue_stats_t queue = collect_queue_stats(queue_dir);
    honch_bench_alloc_stats_t alloc;
    honch_bench_alloc_get(&alloc);

    rate_window_result_t result = {
        .rate = rate,
        .window = window,
        .actual_eps = wall_us == 0u ? 0.0 : ((double)accepted * 1000000.0) / (double)wall_us,
        .events_attempted = attempted,
        .events_accepted = accepted,
        .events_failed = failed,
        .track_avg_us = track_count == 0u ? 0u : sum_samples(track_samples, track_count) / track_count,
        .track_p50_us = percentile(track_samples, track_count, 50u),
        .track_p95_us = percentile(track_samples, track_count, 95u),
        .track_p99_us = percentile(track_samples, track_count, 99u),
        .track_max_us = max_sample(track_samples, track_count),
        .flush_count = flush_count,
        .flush_failures = flush_failures,
        .flush_avg_us = flush_count == 0u ? 0u : sum_samples(flush_samples, flush_count) / flush_count,
        .flush_p95_us = percentile(flush_samples, flush_count, 95u),
        .cpu_percent = wall_us == 0u ? 0.0 : ((double)cpu_elapsed * 100.0) / (double)wall_us,
        .queued_estimate = *queued_estimate,
        .queue = queue,
        .rss_kb = peak_rss_kb(),
        .alloc = alloc,
        .transport_delta = {
            .status = transport->status,
            .calls = transport->calls - transport_before.calls,
            .bytes = transport->bytes - transport_before.bytes,
            .max_body_bytes = transport->max_body_bytes,
        },
    };
    print_rate_window(&result);

    free(track_samples);
    free(flush_samples);
    return status == HONCH_OK ? 0 : 1;
}

static int run_rate_warmup(
    honch_client_t *client,
    const rate_sweep_config_t *config,
    unsigned int rate,
    char *properties,
    size_t properties_size,
    const char *payload,
    size_t *queued_estimate,
    size_t *global_sequence)
{
    if (config->warmup_seconds == 0u) {
        return 0;
    }

    unsigned long long start_us = now_us();
    unsigned long long end_us = start_us + ((unsigned long long)config->warmup_seconds * 1000000ull);
    size_t attempted = 0u;
    size_t accepted_since_flush = 0u;
    while (now_us() < end_us) {
        unsigned long long deadline =
            start_us + (((unsigned long long)attempted * 1000000ull) / (unsigned long long)rate);
        sleep_until_us(deadline);
        snprintf(properties,
                 properties_size,
                 "{\"seq\":%zu,\"target_eps\":%u,\"warmup\":true,\"payload\":\"%s\"}",
                 *global_sequence,
                 rate,
                 payload);
        honch_status_t status = honch_track(client, "bench_rate_warmup", properties);
        attempted++;
        (*global_sequence)++;
        if (status == HONCH_OK) {
            (*queued_estimate)++;
            accepted_since_flush++;
        } else {
            return 1;
        }
        if (accepted_since_flush >= config->flush_every) {
            status = honch_flush(client);
            if (status != HONCH_OK) {
                return 1;
            }
            *queued_estimate = 0u;
            accepted_since_flush = 0u;
        }
    }
    if (*queued_estimate > 0u) {
        honch_status_t status = honch_flush(client);
        if (status != HONCH_OK) {
            return 1;
        }
        *queued_estimate = 0u;
    }
    return 0;
}

static int run_rate_sweep(const rate_sweep_config_t *config)
{
    char queue_dir[PATH_MAX];
    if (make_temp_dir(queue_dir, sizeof(queue_dir)) != 0) {
        return 1;
    }

    size_t payload_size = config->payload_bytes + 1u;
    size_t properties_size = config->payload_bytes + 256u;
    char *payload = (char *)calloc(payload_size, sizeof(*payload));
    char *properties = (char *)calloc(properties_size, sizeof(*properties));
    if (payload == NULL || properties == NULL) {
        free(payload);
        free(properties);
        remove_tree(queue_dir);
        return 1;
    }
    fill_rate_payload(payload, payload_size, config->payload_bytes);

    honch_bench_alloc_reset();
    bench_transport_t transport = {.status = 202L};
    honch_test_set_transport(fake_transport, &transport);

    honch_config_t config_base = bench_config(queue_dir);
    config_base.flush_event_threshold = config->flush_every;
    config_base.disable_background_flush = 1;

    honch_client_t *client = NULL;
    honch_status_t status = honch_init(&client, &config_base);
    if (status != HONCH_OK) {
        honch_test_set_transport(NULL, NULL);
        free(payload);
        free(properties);
        remove_tree(queue_dir);
        return 1;
    }

    printf("BENCH_SUITE_START schema=honch-rate-sweep-v1 runtime=c-core-posix "
           "mode=sdk-only rates=");
    for (size_t i = 0u; i < config->rate_count; i++) {
        printf("%s%u", i == 0u ? "" : ",", config->rates[i]);
    }
    printf(" duration_seconds=%u warmup_seconds=%u window_seconds=%u "
           "payload_bytes=%zu flush_every=%zu\n",
           config->duration_seconds,
           config->warmup_seconds,
           config->window_seconds,
           config->payload_bytes,
           config->flush_every);

    int rc = 0;
    size_t queued_estimate = 0u;
    size_t global_sequence = 0u;
    for (size_t i = 0u; i < config->rate_count && rc == 0; i++) {
        unsigned int rate = config->rates[i];
        printf("BENCH_RATE_START runtime=c-core-posix mode=sdk-only rate_target_eps=%u "
               "duration_seconds=%u warmup_seconds=%u window_seconds=%u\n",
               rate,
               config->duration_seconds,
               config->warmup_seconds,
               config->window_seconds);

        rc = run_rate_warmup(
            client,
            config,
            rate,
            properties,
            properties_size,
            payload,
            &queued_estimate,
            &global_sequence);
        unsigned int windows = config->duration_seconds / config->window_seconds;
        if ((config->duration_seconds % config->window_seconds) != 0u) {
            windows++;
        }
        for (unsigned int window = 1u; rc == 0 && window <= windows; window++) {
            rc = run_rate_window(
                client,
                config,
                rate,
                window,
                properties,
                properties_size,
                payload,
                &transport,
                queue_dir,
                &queued_estimate,
                &global_sequence);
        }

        unsigned long long final_flush_start = now_us();
        honch_status_t flush_status = honch_flush(client);
        unsigned long long final_flush_us = now_us() - final_flush_start;
        if (flush_status == HONCH_OK) {
            queued_estimate = 0u;
        } else {
            rc = 1;
        }
        printf("BENCH_RATE_END runtime=c-core-posix mode=sdk-only rate_target_eps=%u "
               "status=%s final_flush_us=%llu queued_estimate=%zu\n",
               rate,
               honch_status_string(flush_status),
               final_flush_us,
               queued_estimate);
    }

    printf("BENCH_SUITE_END schema=honch-rate-sweep-v1 runtime=c-core-posix "
           "mode=sdk-only status=%s transport_calls=%zu transport_bytes=%zu\n",
           rc == 0 ? "ok" : "failed",
           transport.calls,
           transport.bytes);

    if (client != NULL) {
        (void)honch_shutdown(client);
    }
    honch_test_set_transport(NULL, NULL);
    free(payload);
    free(properties);
    remove_tree(queue_dir);
    return rc;
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
    printf("name,phase,iterations,events_per_iteration,total_us,mean_us,min_us,p50_us,p95_us,p99_us,max_us,peak_rss_kb,sdk_current_bytes,sdk_peak_bytes,sdk_total_allocated_bytes,sdk_total_freed_bytes,sdk_malloc_calls,sdk_calloc_calls,sdk_realloc_calls,sdk_free_calls,sdk_failed_allocations,sdk_live_allocations,sdk_peak_live_allocations,transport_calls,transport_bytes,transport_max_body_bytes,queue_pending_files,queue_pending_bytes,queue_dead_files,queue_dead_bytes,status\n");
}

static void print_result(const bench_result_t *result)
{
    double mean = result->iterations == 0u ? 0.0 : (double)result->total_us / (double)result->iterations;
    printf("%s,%s,%zu,%zu,%llu,%.2f,%llu,%llu,%llu,%llu,%llu,%ld,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%s\n",
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
        (void)honch_shutdown(client);
    }
    honch_test_set_transport(NULL, NULL);
    remove_tree(queue_dir);
    free(samples);
    return status == HONCH_OK ? 0 : 1;
}

static int run_flush_scenario(const char *name, size_t events, long http_status)
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
        honch_test_set_transport(fake_transport, &(bench_transport_t){.status = 202L});
        (void)honch_shutdown(client);
    }
    honch_test_set_transport(NULL, NULL);
    remove_tree(queue_dir);
    return (http_status >= 200L && http_status < 300L) ? (status == HONCH_OK ? 0 : 1) : 0;
}

int main(int argc, char **argv)
{
    rate_sweep_config_t rate_sweep_config;
    int rate_sweep = 0;
    if (parse_rate_sweep_args(argc, argv, &rate_sweep_config, &rate_sweep) != 0) {
        print_rate_sweep_usage(argv[0]);
        return 2;
    }
    if (rate_sweep) {
        return run_rate_sweep(&rate_sweep_config);
    }

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
    if (run_flush_scenario("flush_1_success", 1u, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_success", 50u, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_200_success", 200u, 202L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_retry_500", 50u, 500L) != 0) {
        return 1;
    }
    if (run_flush_scenario("flush_50_rejected_400", 50u, 400L) != 0) {
        return 1;
    }

    return 0;
}
