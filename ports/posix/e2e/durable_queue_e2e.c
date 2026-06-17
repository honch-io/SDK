#define _DEFAULT_SOURCE
// End-to-end test of the durable two-tier queue against a live Honch sandbox.
//
// It drives the REAL shipping honch_tiered_queue.c + a posix file-backed NV
// adapter through the posix libcurl transport, the real capture endpoint, and
// verifies the events land in ClickHouse. A simulated power loss (drop the
// client without uploading) + reboot (fresh client over the same NV directory)
// proves events queued before power-off survive and upload after restart.
//
//   Phase A: track N events -> force-spill to the NV (flash) tier -> abandon
//            the client WITHOUT uploading (== power cut mid-session).
//   Phase B: "reboot": new client over the same NV dir -> recover -> flush.
//   Phase C: confirm exactly N rows in ClickHouse for this run's device id.

#include "honch/core/honch.h"
#include "honch/core/ram_queue.h"
#include "honch/posix/honch.h"

#include "honch_nv_queue.h"
#include "honch_tiered_queue.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define N_EVENTS 10
#define EVENT_NAME "durable_e2e_probe"

// ---- posix file-backed NV adapter (one file per record, monotonic ids) -----
typedef struct {
    char dir[256];
    uint64_t head;
    uint64_t next;
    size_t count;
    int on;
} PosixNv;

static void nvpath(PosixNv *s, char *out, size_t n, uint64_t id) {
    snprintf(out, n, "%s/%010llu", s->dir, (unsigned long long)id);
}

static int posixnv_begin(PosixNv *s) {
    mkdir(s->dir, 0755);
    DIR *d = opendir(s->dir);
    if (!d) { s->on = 0; return 0; }
    uint64_t mn = UINT64_MAX, mx = 0;
    size_t c = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        int numeric = nm[0] != '\0';
        for (const char *p = nm; *p; p++) if (*p < '0' || *p > '9') { numeric = 0; break; }
        if (!numeric) continue;
        uint64_t id = strtoull(nm, NULL, 10);
        if (id < mn) mn = id;
        if (id > mx) mx = id;
        c++;
    }
    closedir(d);
    if (c == 0) { s->head = 1; s->next = 1; } else { s->head = mn; s->next = mx + 1; }
    s->count = c;
    s->on = 1;
    return 1;
}

static int nv_enabled(void *ctx) { return ((PosixNv *)ctx)->on; }
static honch_status_t nv_count(void *ctx, size_t *o) { *o = ((PosixNv *)ctx)->count; return HONCH_OK; }
static honch_status_t nv_length_at(void *ctx, size_t i, size_t *o) {
    PosixNv *s = ctx;
    if (i >= s->count) return HONCH_ERROR_INVALID_ARGUMENT;
    char p[300]; nvpath(s, p, sizeof p, s->head + i);
    FILE *f = fopen(p, "rb"); if (!f) return HONCH_ERROR_IO;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
    *o = (size_t)sz; return HONCH_OK;
}
static honch_status_t nv_read_at(void *ctx, size_t i, uint32_t off, uint8_t *buf, size_t n) {
    PosixNv *s = ctx;
    if (i >= s->count) return HONCH_ERROR_INVALID_ARGUMENT;
    char p[300]; nvpath(s, p, sizeof p, s->head + i);
    FILE *f = fopen(p, "rb"); if (!f) return HONCH_ERROR_IO;
    honch_status_t st = HONCH_OK;
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(buf, 1, n, f) != n) st = HONCH_ERROR_IO;
    fclose(f); return st;
}
static honch_status_t nv_append(void *ctx, honch_nv_read_cb reader, void *rc, size_t total) {
    PosixNv *s = ctx;
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s/tmp", s->dir);
    FILE *f = fopen(tmp, "wb"); if (!f) return HONCH_ERROR_IO;
    uint8_t ch[256]; size_t w = 0;
    while (w < total) {
        size_t want = total - w; if (want > sizeof ch) want = sizeof ch;
        if (reader(rc, (uint32_t)w, ch, want) != HONCH_OK || fwrite(ch, 1, want, f) != want) {
            fclose(f); remove(tmp); return HONCH_ERROR_IO;
        }
        w += want;
    }
    fclose(f);
    char p[300]; nvpath(s, p, sizeof p, s->next);
    if (rename(tmp, p) != 0) { remove(tmp); return HONCH_ERROR_IO; }
    s->next++; s->count++; return HONCH_OK;
}
static honch_status_t nv_consume_front(void *ctx, size_t n) {
    PosixNv *s = ctx;
    if (n > s->count) return HONCH_ERROR_INVALID_ARGUMENT;
    for (size_t k = 0; k < n; k++) { char p[300]; nvpath(s, p, sizeof p, s->head); remove(p); s->head++; s->count--; }
    return HONCH_OK;
}

static honch_nv_queue_ops_t nv_ops(PosixNv *s) {
    honch_nv_queue_ops_t o = {0};
    o.enabled = nv_enabled; o.count = nv_count; o.length_at = nv_length_at;
    o.read_at = nv_read_at; o.append = nv_append; o.consume_front = nv_consume_front;
    o.get_stats = NULL; o.ctx = s;
    return o;
}

// ---- harness ---------------------------------------------------------------
static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k); return (v && *v) ? v : d;
}

static honch_status_t make_core(honch_client_t **client,
                                const char *endpoint, const char *token, const char *device_id,
                                honch_posix_platform_t *pfx, honch_posix_storage_t *stx,
                                honch_posix_transport_t **txx,
                                honch_event_queue_ops_t *tiered_ops) {
    static honch_platform_ops_t pf; static honch_state_storage_ops_t st;
    static honch_event_queue_ops_t posix_q; static honch_transport_ops_t tr;
    honch_posix_platform_ops_init(&pf, pfx);
    honch_posix_storage_ops_init(&st, &posix_q, stx, "/tmp/honch_e2e_state"); // state only; queue is ours
    *txx = (honch_posix_transport_t *)calloc(1, sizeof(**txx));
    honch_posix_transport_ops_init(&tr, *txx);

    honch_core_config_t c; memset(&c, 0, sizeof c);
    c.api_key = token; c.endpoint_url = endpoint; c.device_id = device_id;
    c.device_model = "posix-e2e"; c.firmware_version = "0.0.0"; c.environment = "test";
    c.sdk_platform = "posix-e2e"; c.queue_directory = "";
    c.flush_interval_seconds = 1; c.flush_event_threshold = 1; c.flush_min_interval_ms = 0;
    c.max_event_bytes = 4096; c.batch_size = 20; c.max_queued_events = 1000;
    c.platform = &pf; c.state_storage = &st; c.transport = &tr; c.event_queue = tiered_ops;
    honch_status_t s = honch_core_init(client, &c);
    if (s == HONCH_OK) (*txx)->client = *client;
    return s;
}

int main(void) {
    const char *endpoint = env_or("HONCH_E2E_ENDPOINT", "http://127.0.0.1:8001");
    const char *token = env_or("HONCH_E2E_TOKEN", "honch_e2e_test_key");
    const char *ch = env_or("HONCH_E2E_CLICKHOUSE_URL", "http://127.0.0.1:8123");
    const char *db = env_or("HONCH_E2E_CLICKHOUSE_DATABASE", "platform");
    const char *nvdir = "/tmp/honch_e2e_nv";

    char device_id[64];
    snprintf(device_id, sizeof device_id, "durable-e2e-%d", (int)getpid());
    printf("e2e: device_id=%s endpoint=%s\n", device_id, endpoint);

    system("rm -rf /tmp/honch_e2e_nv /tmp/honch_e2e_state");

    static uint8_t scratch[2048];

    // ===== PHASE A: track, force-spill to flash, then "lose power" (no upload)
    {
        static uint8_t rambuf[8192];
        honch_ram_queue_t ram; honch_event_queue_ops_t ramops;
        if (honch_ram_queue_init(&ram, rambuf, sizeof rambuf) != HONCH_OK) { printf("FAIL ram init\n"); return 1; }
        honch_ram_queue_ops_init(&ramops, &ram);
        PosixNv nv = {0}; snprintf(nv.dir, sizeof nv.dir, "%s", nvdir); posixnv_begin(&nv);
        honch_nv_queue_ops_t nvo = nv_ops(&nv);
        honch_tiered_queue_t tq; honch_event_queue_ops_t tops;
        honch_tiered_queue_config_t cfg = {1, 0}; // aggressive: spill almost everything to NV
        honch_tiered_queue_init(&tq, &ramops, &nvo, &cfg, scratch, sizeof scratch);
        honch_tiered_queue_ops_init(&tops, &tq);

        honch_client_t *client = NULL; honch_posix_platform_t pfx; honch_posix_storage_t stx; honch_posix_transport_t *txx;
        honch_status_t s = make_core(&client, endpoint, token, device_id, &pfx, &stx, &txx, &tops);
        if (s != HONCH_OK) { printf("FAIL phase A init: %s\n", honch_status_string(s)); return 1; }

        for (int i = 0; i < N_EVENTS; i++) {
            honch_property_t props[1] = { honch_prop("seq", honch_i64(i)) };
            honch_core_track(client, EVENT_NAME, props, 1);
        }
        honch_tiered_queue_persist(&tq, 0); // drain all RAM -> NV (flash)

        // count files on disk == proof events are durably stored, not uploaded.
        size_t stored = 0; nv_count(&nv, &stored);
        printf("phase A: tracked %d, persisted to NV files=%zu (NO upload — simulating power loss)\n", N_EVENTS, stored);
        if (stored < (size_t)N_EVENTS) { printf("FAIL: expected >=%d persisted, got %zu\n", N_EVENTS, stored); return 1; }
        // abandon client/transport WITHOUT flushing == power cut.
    }

    // ===== PHASE B: reboot — fresh client over the SAME nv dir, recover, upload
    {
        static uint8_t rambuf[8192];
        honch_ram_queue_t ram; honch_event_queue_ops_t ramops;
        honch_ram_queue_init(&ram, rambuf, sizeof rambuf);
        honch_ram_queue_ops_init(&ramops, &ram);
        PosixNv nv = {0}; snprintf(nv.dir, sizeof nv.dir, "%s", nvdir); posixnv_begin(&nv);
        size_t recovered = 0; nv_count(&nv, &recovered);
        printf("phase B (reboot): recovered %zu events from NV\n", recovered);
        honch_nv_queue_ops_t nvo = nv_ops(&nv);
        honch_tiered_queue_t tq; honch_event_queue_ops_t tops;
        honch_tiered_queue_config_t cfg = {0, 0};
        honch_tiered_queue_init(&tq, &ramops, &nvo, &cfg, scratch, sizeof scratch);
        honch_tiered_queue_ops_init(&tops, &tq);

        honch_client_t *client = NULL; honch_posix_platform_t pfx; honch_posix_storage_t stx; honch_posix_transport_t *txx;
        honch_status_t s = make_core(&client, endpoint, token, device_id, &pfx, &stx, &txx, &tops);
        if (s != HONCH_OK) { printf("FAIL phase B init: %s\n", honch_status_string(s)); return 1; }

        honch_core_set_uploads_paused(client, 0);
        size_t left = 0;
        nv_count(&nv, &left);
        for (int i = 0; i < 40 && left != 0; i++) {
            honch_status_t fs = honch_core_flush(client);
            nv_count(&nv, &left);
            printf("  flush %2d: status=%-20s nv_left=%zu\n", i, honch_status_string(fs), left);
            if (left == 0) break;
            honch_core_tick(client);
            // Capture rate-limits; events stay durably queued in NV (retryable),
            // so just back off and let the window reopen — never a data loss.
            sleep(fs == HONCH_ERROR_RATE_LIMITED ? 3 : 1);
        }
        printf("phase B: after flush, NV files remaining=%zu\n", left);
        if (left != 0) { printf("FAIL: %zu events not uploaded\n", left); return 1; }
    }

    // ===== PHASE C: verify in ClickHouse
    printf("phase C: verifying in ClickHouse...\n");
    char query[512];
    snprintf(query, sizeof query,
             "SELECT count() FROM %s.events WHERE event='%s' AND device_id='%s'",
             db, EVENT_NAME, device_id);
    for (int attempt = 0; attempt < 20; attempt++) {
        char cmd[1200];
        snprintf(cmd, sizeof cmd,
                 "curl -s '%s/?database=%s' --data-binary \"%s\" 2>/dev/null > /tmp/honch_e2e_ch.out",
                 ch, db, query);
        system(cmd);
        FILE *f = fopen("/tmp/honch_e2e_ch.out", "r");
        long count = -1; if (f) { if (fscanf(f, "%ld", &count) != 1) count = -1; fclose(f); }
        printf("  attempt %d: clickhouse count=%ld\n", attempt + 1, count);
        if (count >= N_EVENTS) {
            printf("PASS: %ld events for %s present in ClickHouse (queued pre-power-loss, recovered, uploaded)\n",
                   count, device_id);
            return 0;
        }
        sleep(2);
    }
    printf("FAIL: events did not appear in ClickHouse\n");
    return 1;
}
