/* H4 regression: the POSIX queue must make acknowledged deletions durable.
 *
 * Enqueue already fsyncs the pending directory after the rename so a queued
 * event survives a power cut. The removal side (consume after Capture accepts,
 * dead-letter, and drop-oldest eviction) historically did NOT fsync the
 * directory, so after a crash an already-accepted-and-unlinked event could
 * reappear (duplicate delivery) or a dead-lettered file could resurface in
 * pending. This test pins the contract: in SYNC_ALWAYS mode the directory whose
 * entries changed is fsync'd on delete/rename, and in OS_BUFFERED mode it is not.
 *
 * It observes fsync by interposing the libc symbol (a strong definition here
 * overrides libc for the statically-linked port objects) and classifying each
 * call as a directory or file sync via fstat. The interposer is a no-op: the
 * data is already flushed by fclose, and crash-durability itself is not under
 * test -- only whether the durability barrier is issued.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "honch_internal.h"        /* core honch_client_t (queue ops ctx) */
#include "honch/core/storage.h"
#include "honch/posix/honch.h"

/* ---- fsync interposer ---------------------------------------------------- */
static int g_dir_fsync = 0;
static int g_file_fsync = 0;

int fsync(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        g_dir_fsync++;
    } else {
        g_file_fsync++;
    }
    return 0; /* no-op: fclose already flushed; crash-durability not under test */
}

/* ---- harness ------------------------------------------------------------- */
static char g_pending[320];
static char g_dead[320];
static char g_state[320];
static honch_posix_storage_t g_storage_ctx;

static void make_dirs(void)
{
    char base[256];
    snprintf(base, sizeof base, "/tmp/honch_h4_%d", (int)getpid());
    snprintf(g_pending, sizeof g_pending, "%s/pending", base);
    snprintf(g_dead, sizeof g_dead, "%s/dead", base);
    snprintf(g_state, sizeof g_state, "%s/state", base);
    mkdir(base, 0755);
    mkdir(g_pending, 0755);
    mkdir(g_dead, 0755);
    mkdir(g_state, 0755);
}

static int count_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void clean_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

static void init_client(honch_client_t *c, honch_event_queue_ops_t *q, honch_durability_mode_t mode)
{
    clean_dir(g_pending);
    clean_dir(g_dead);

    memset(c, 0, sizeof *c);
    c->pending_directory = g_pending;
    c->dead_directory = g_dead;
    c->state_directory = g_state;
    c->durability_mode = mode;
    c->max_event_bytes = 4096u;
    c->max_queued_events = 100u;
    c->queued_event_count = 0u;
    c->sequence = 0u;
    c->active_storage_reader_sequence = UINT64_MAX;

    honch_state_storage_ops_t state_ops;
    assert(honch_posix_storage_ops_init(&state_ops, q, &g_storage_ctx, g_pending) == HONCH_OK);
    q->ctx = c; /* the queue ops operate on the core client */
}

/* T1: consume after acceptance fsyncs the pending directory (SYNC_ALWAYS).
 * Also proves the interposer sees the port's fsyncs: the enqueue itself must
 * issue a directory fsync on the current code. */
static void test_consume_fsyncs_dir_sync_always(void)
{
    honch_client_t c;
    honch_event_queue_ops_t q;
    init_client(&c, &q, HONCH_DURABILITY_SYNC_ALWAYS);

    const uint8_t ev[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    g_dir_fsync = 0;
    assert(q.queue_push(&c, ev, sizeof ev, 1u) == HONCH_OK);
    assert(count_files(g_pending) == 1);
    assert(g_dir_fsync >= 1); /* sanity: interposer observes the enqueue's dir fsync */

    g_dir_fsync = 0;
    assert(q.queue_consume(&c, 1u) == HONCH_OK);
    assert(count_files(g_pending) == 0);
    assert(g_dir_fsync >= 1); /* the acknowledged delete must be made durable */
    printf("  T1 consume fsyncs pending dir (SYNC_ALWAYS) OK\n");
}

/* T2: dead-letter (rename pending -> dead) fsyncs in SYNC_ALWAYS. */
static void test_dead_letter_fsyncs_dir_sync_always(void)
{
    honch_client_t c;
    honch_event_queue_ops_t q;
    init_client(&c, &q, HONCH_DURABILITY_SYNC_ALWAYS);

    const uint8_t ev[8] = {0};
    assert(q.queue_push(&c, ev, sizeof ev, 7u) == HONCH_OK);

    g_dir_fsync = 0;
    assert(q.queue_dead_letter(&c, 7u) == HONCH_OK);
    assert(count_files(g_pending) == 0);
    assert(count_files(g_dead) == 1);
    assert(g_dir_fsync >= 1); /* the rename out of pending must be made durable */
    printf("  T2 dead_letter fsyncs dir (SYNC_ALWAYS) OK\n");
}

/* T3: OS_BUFFERED mode must NOT add directory fsyncs on delete. */
static void test_consume_no_fsync_os_buffered(void)
{
    honch_client_t c;
    honch_event_queue_ops_t q;
    init_client(&c, &q, HONCH_DURABILITY_OS_BUFFERED);

    const uint8_t ev[8] = {0};
    assert(q.queue_push(&c, ev, sizeof ev, 1u) == HONCH_OK);

    g_dir_fsync = 0;
    g_file_fsync = 0;
    assert(q.queue_consume(&c, 1u) == HONCH_OK);
    assert(count_files(g_pending) == 0);
    assert(g_dir_fsync == 0); /* OS_BUFFERED trades durability for speed */
    printf("  T3 consume issues no fsync in OS_BUFFERED OK\n");
}

// H5: drop-oldest must evict the lowest-sequence event, not the lexically-first
// filename. Filenames are <timestamp>-<sequence>-<id>, and the sequence field is
// not fixed-width, so once sequences pass the padded width (or the clock skews)
// lexical filename order diverges from true sequence order.
static void test_drop_oldest_evicts_lowest_sequence(void)
{
    honch_client_t c;
    honch_event_queue_ops_t q;
    init_client(&c, &q, HONCH_DURABILITY_OS_BUFFERED);
    c.max_queued_events = 3u;
    c.queued_event_count = 3u;

    // Same timestamp prefix; sequences straddle the 6-digit boundary, so lexical
    // order ("1000000" < "999998" < "999999") inverts true sequence order.
    const char *names[3] = {
        "00000000000000000001-999998-a.hqe",   // true oldest (min sequence)
        "00000000000000000001-999999-b.hqe",
        "00000000000000000001-1000000-c.hqe",  // sorts first lexically, but newest
    };
    for (int i = 0; i < 3; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", g_pending, names[i]);
        FILE *f = fopen(path, "wb");
        assert(f != NULL);
        fputc('x', f);
        fclose(f);
    }

    // Enqueue a 4th event -> over capacity -> exactly one eviction.
    const uint8_t ev[8] = {0};
    assert(q.queue_push(&c, ev, sizeof ev, 1000001u) == HONCH_OK);

    char p998[512], p999[512], p1m[512];
    snprintf(p998, sizeof p998, "%s/%s", g_pending, names[0]);
    snprintf(p999, sizeof p999, "%s/%s", g_pending, names[1]);
    snprintf(p1m, sizeof p1m, "%s/%s", g_pending, names[2]);

    assert(access(p998, F_OK) != 0);  // sequence 999998: true oldest -> evicted
    assert(access(p999, F_OK) == 0);  // kept
    assert(access(p1m, F_OK) == 0);   // kept (newer, despite sorting first lexically)
    assert(count_files(g_pending) == 3);
    printf("  H5 drop-oldest evicts lowest sequence OK\n");
}

// M11: a state value containing an embedded NUL must be rejected, not silently
// truncated (which would persist a corrupted device_id / distinct_id).
static void test_state_get_rejects_embedded_nul(void)
{
    honch_client_t c;
    honch_event_queue_ops_t q;
    init_client(&c, &q, HONCH_DURABILITY_OS_BUFFERED);

    honch_state_storage_ops_t state_ops;
    honch_event_queue_ops_t throwaway_q;
    honch_posix_storage_t storage_ctx;
    assert(honch_posix_storage_ops_init(&state_ops, &throwaway_q, &storage_ctx, g_pending) == HONCH_OK);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_state, "device_id");

    // A clean value round-trips.
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fwrite("abcd", 1, 4, f);
    fclose(f);
    uint8_t buf[64];
    size_t sz = sizeof buf;
    assert(state_ops.state_get(&c, "device_id", buf, &sz) == HONCH_OK);
    assert(sz == 4);

    // An embedded NUL must be rejected, not truncated to "ab".
    f = fopen(path, "wb");
    assert(f != NULL);
    fwrite("ab\0cd", 1, 5, f);
    fclose(f);
    sz = sizeof buf;
    assert(state_ops.state_get(&c, "device_id", buf, &sz) != HONCH_OK);
    printf("  M11 state_get rejects embedded NUL OK\n");
}

int main(void)
{
    make_dirs();
    test_consume_fsyncs_dir_sync_always();
    test_dead_letter_fsyncs_dir_sync_always();
    test_consume_no_fsync_os_buffered();
    test_drop_oldest_evicts_lowest_sequence();
    test_state_get_rejects_embedded_nul();
    printf("ALL DURABLE DELETE TESTS PASSED\n");
    return 0;
}
