#include "honch_internal.h"

#include <assert.h>
#include <string.h>

static uint8_t g_blob[10240];
static size_t g_blob_size = sizeof(g_blob);
static int g_read_calls = 0;

static size_t fake_size(void *ctx)
{
    (void)ctx;
    return g_blob_size;
}

static int fake_read(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    (void)ctx;
    g_read_calls++;
    if (offset + len > g_blob_size) {
        return -1;
    }
    memcpy(out, g_blob + offset, len);
    return (int)len;
}

/* The image streams off in fixed-size chunks, never exceeding the caller's buffer,
 * and reassembles byte-for-byte. */
static void test_chunks_reassemble_full_image_bounded_by_buffer(void)
{
    for (size_t i = 0u; i < sizeof(g_blob); i++) {
        g_blob[i] = (uint8_t)(i * 31u + 7u);
    }
    g_blob_size = sizeof(g_blob);
    g_read_calls = 0;

    honch_coredump_source_t source = {.size = fake_size, .read = fake_read, .clear = NULL, .ctx = NULL};
    uint8_t reassembled[sizeof(g_blob)];
    uint8_t chunk[1024];
    size_t offset = 0u;

    for (;;) {
        size_t got = 0u;
        assert(honch_coredump_chunk(&source, offset, chunk, sizeof(chunk), &got) == HONCH_OK);
        if (got == 0u) {
            break;
        }
        assert(got <= sizeof(chunk)); /* never exceeds the bounded buffer */
        memcpy(reassembled + offset, chunk, got);
        offset += got;
    }

    assert(offset == sizeof(g_blob));
    assert(memcmp(reassembled, g_blob, sizeof(g_blob)) == 0);
    assert(g_read_calls == 10); /* 10240 / 1024 */
}

/* A non-multiple image yields a short final chunk, then EOF (got == 0). */
static void test_final_partial_chunk_then_eof(void)
{
    g_blob_size = 2500u;
    honch_coredump_source_t source = {.size = fake_size, .read = fake_read, .clear = NULL, .ctx = NULL};
    uint8_t chunk[1024];
    size_t got = 0u;

    assert(honch_coredump_chunk(&source, 0u, chunk, sizeof(chunk), &got) == HONCH_OK && got == 1024u);
    assert(honch_coredump_chunk(&source, 1024u, chunk, sizeof(chunk), &got) == HONCH_OK && got == 1024u);
    assert(honch_coredump_chunk(&source, 2048u, chunk, sizeof(chunk), &got) == HONCH_OK && got == 452u);
    assert(honch_coredump_chunk(&source, 2500u, chunk, sizeof(chunk), &got) == HONCH_OK && got == 0u);

    g_blob_size = sizeof(g_blob);
}

static void test_rejects_bad_args(void)
{
    honch_coredump_source_t source = {.size = fake_size, .read = fake_read, .clear = NULL, .ctx = NULL};
    uint8_t chunk[16];
    size_t got = 0u;

    assert(honch_coredump_chunk(NULL, 0u, chunk, sizeof(chunk), &got) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_coredump_chunk(&source, 0u, NULL, sizeof(chunk), &got) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_coredump_chunk(&source, 0u, chunk, 0u, &got) == HONCH_ERROR_INVALID_ARGUMENT);
    assert(honch_coredump_chunk(&source, sizeof(g_blob) + 1u, chunk, sizeof(chunk), &got) ==
        HONCH_ERROR_INVALID_ARGUMENT);
}

int main(void)
{
    test_chunks_reassemble_full_image_bounded_by_buffer();
    test_final_partial_chunk_then_eof();
    test_rejects_bad_args();
    return 0;
}
