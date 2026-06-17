#include "honch_tiered_queue.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* NV record framing: [ seq:u64 LE ][ len:u32 LE ][ payload ][ crc16 LE ]. */
#define HONCH_NV_REC_HEADER 12u /* seq(8) + len(4) */
#define HONCH_NV_REC_FOOTER 2u  /* crc16 */
#define HONCH_NV_REC_OVERHEAD (HONCH_NV_REC_HEADER + HONCH_NV_REC_FOOTER)

/* ---- little-endian + CRC helpers ---------------------------------------- */

static void put_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_u32le(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t get_u32le(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). */
static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int tq_nv_active(const honch_tiered_queue_t *tq) {
    return tq->nv_ops != NULL && tq->nv_ops->enabled != NULL && tq->nv_ops->enabled(tq->nv_ops->ctx);
}

static honch_status_t tq_nv_count(const honch_tiered_queue_t *tq, size_t *out) {
    return tq->nv_ops->count(tq->nv_ops->ctx, out);
}

/* True if the framed record bytes in `buf` (length rec_len) are well-formed. */
static int record_valid(const uint8_t *buf, size_t rec_len) {
    if (rec_len < HONCH_NV_REC_OVERHEAD) return 0;
    uint32_t len = get_u32le(buf + 8);
    if ((size_t)len != rec_len - HONCH_NV_REC_OVERHEAD) return 0;
    uint16_t crc = crc16_update(0xFFFFu, buf, HONCH_NV_REC_HEADER + (size_t)len);
    return crc == get_u16le(buf + HONCH_NV_REC_HEADER + (size_t)len);
}

/* ---- spill: serve a framed record to nv_ops.append via a reader callback -- */

typedef struct {
    uint8_t header[HONCH_NV_REC_HEADER];
    const uint8_t *payload;
    size_t payload_len;
    uint8_t footer[HONCH_NV_REC_FOOTER];
} tq_spill_src_t;

static honch_status_t tq_spill_reader(void *cb_ctx, uint32_t offset, uint8_t *buf, size_t n) {
    tq_spill_src_t *s = (tq_spill_src_t *)cb_ctx;
    size_t total = HONCH_NV_REC_HEADER + s->payload_len + HONCH_NV_REC_FOOTER;
    if ((size_t)offset > total || n > total - (size_t)offset) return HONCH_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < n; i++) {
        size_t pos = (size_t)offset + i;
        if (pos < HONCH_NV_REC_HEADER) {
            buf[i] = s->header[pos];
        } else if (pos < HONCH_NV_REC_HEADER + s->payload_len) {
            buf[i] = s->payload[pos - HONCH_NV_REC_HEADER];
        } else {
            buf[i] = s->footer[pos - HONCH_NV_REC_HEADER - s->payload_len];
        }
    }
    return HONCH_OK;
}

honch_status_t honch_tiered_queue_persist(honch_tiered_queue_t *tq, size_t target_bytes) {
    if (tq == NULL) return HONCH_ERROR_INVALID_ARGUMENT;
    if (!tq_nv_active(tq) || tq->nv_ops->append == NULL) return HONCH_OK;

    for (;;) {
        honch_queue_stats_t st;
        if (tq->ram_ops.queue_get_stats(tq->ram_ops.ctx, &st) != HONCH_OK) break;
        if (st.queued_events == 0u || st.queued_bytes <= target_bytes) break;

        honch_storage_event_t ev[1] = {{0}};
        size_t n = 0;
        if (tq->ram_ops.queue_read_batch(tq->ram_ops.ctx, ev, 1u, (size_t)-1, &n) != HONCH_OK || n == 0u) {
            break;
        }

        tq_spill_src_t src;
        put_u64le(src.header, ev[0].sequence);
        put_u32le(src.header + 8, (uint32_t)ev[0].length);
        src.payload = ev[0].data;
        src.payload_len = ev[0].length;
        uint16_t crc = crc16_update(0xFFFFu, src.header, HONCH_NV_REC_HEADER);
        crc = crc16_update(crc, src.payload, src.payload_len);
        src.footer[0] = (uint8_t)(crc & 0xFFu);
        src.footer[1] = (uint8_t)((crc >> 8) & 0xFFu);

        size_t total = HONCH_NV_REC_HEADER + src.payload_len + HONCH_NV_REC_FOOTER;
        if (tq->nv_ops->append(tq->nv_ops->ctx, tq_spill_reader, &src, total) != HONCH_OK) break;
        if (tq->ram_ops.queue_consume(tq->ram_ops.ctx, ev[0].sequence) != HONCH_OK) break;
    }
    return HONCH_OK;
}

/* ---- composed event_queue_ops ------------------------------------------- */

static honch_status_t tq_push(void *ctx, const uint8_t *event, size_t event_size, uint64_t sequence) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    honch_status_t status = tq->ram_ops.queue_push(tq->ram_ops.ctx, event, event_size, sequence);
    if (status != HONCH_OK) return status;
    if (tq->config.spill_high_water_bytes > 0u && tq_nv_active(tq)) {
        honch_queue_stats_t st;
        if (tq->ram_ops.queue_get_stats(tq->ram_ops.ctx, &st) == HONCH_OK &&
            st.queued_bytes >= tq->config.spill_high_water_bytes) {
            honch_tiered_queue_persist(tq, tq->config.spill_low_water_bytes);
        }
    }
    return status;
}

/* Drop any corrupt records sitting at the NV head (e.g. a torn write). */
static void tq_drop_corrupt_head(honch_tiered_queue_t *tq) {
    for (;;) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) != HONCH_OK || cnt == 0u) return;
        size_t rec_len = 0;
        if (tq->nv_ops->length_at(tq->nv_ops->ctx, 0u, &rec_len) != HONCH_OK) return;
        if (rec_len < HONCH_NV_REC_OVERHEAD || rec_len > tq->read_scratch_size) {
            tq->nv_ops->consume_front(tq->nv_ops->ctx, 1u);
            continue;
        }
        if (tq->nv_ops->read_at(tq->nv_ops->ctx, 0u, 0u, tq->read_scratch, rec_len) != HONCH_OK) {
            tq->nv_ops->consume_front(tq->nv_ops->ctx, 1u);
            continue;
        }
        if (record_valid(tq->read_scratch, rec_len)) return; /* head is good */
        tq->nv_ops->consume_front(tq->nv_ops->ctx, 1u);
    }
}

static honch_status_t tq_read_batch_nv(honch_tiered_queue_t *tq, honch_storage_event_t *events,
                                       size_t max_events, size_t max_event_bytes, size_t *event_count) {
    tq_drop_corrupt_head(tq);

    size_t cnt = 0;
    if (tq_nv_count(tq, &cnt) != HONCH_OK) return HONCH_ERROR_IO;

    size_t cursor = 0, n = 0, bytes = 0;
    for (size_t idx = 0; idx < cnt && n < max_events; idx++) {
        size_t rec_len = 0;
        if (tq->nv_ops->length_at(tq->nv_ops->ctx, idx, &rec_len) != HONCH_OK) break;
        if (rec_len < HONCH_NV_REC_OVERHEAD) break;
        size_t plen = rec_len - HONCH_NV_REC_OVERHEAD;
        if (plen > max_event_bytes) break;
        if (n > 0u && plen > max_event_bytes - bytes) break;
        if (rec_len > tq->read_scratch_size - cursor) break;
        if (tq->nv_ops->read_at(tq->nv_ops->ctx, idx, 0u, tq->read_scratch + cursor, rec_len) != HONCH_OK) break;
        if (!record_valid(tq->read_scratch + cursor, rec_len)) break;

        events[n].data = tq->read_scratch + cursor + HONCH_NV_REC_HEADER;
        events[n].length = plen;
        events[n].sequence = get_u64le(tq->read_scratch + cursor);
        events[n].flags = HONCH_STORAGE_EVENT_BORROWED;
        cursor += rec_len;
        bytes += plen;
        n++;
    }
    *event_count = n;
    return HONCH_OK;
}

static honch_status_t tq_read_batch(void *ctx, honch_storage_event_t *events, size_t max_events,
                                    size_t max_event_bytes, size_t *event_count) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (event_count == NULL) return HONCH_ERROR_INVALID_ARGUMENT;
    *event_count = 0u;
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK && cnt > 0u) {
            return tq_read_batch_nv(tq, events, max_events, max_event_bytes, event_count);
        }
    }
    return tq->ram_ops.queue_read_batch(tq->ram_ops.ctx, events, max_events, max_event_bytes, event_count);
}

/* Returns 1 and consumes the NV head if its sequence matches `sequence`. */
static int tq_nv_consume_if_head(honch_tiered_queue_t *tq, uint64_t sequence) {
    if (!tq_nv_active(tq)) return 0;
    size_t cnt = 0;
    if (tq_nv_count(tq, &cnt) != HONCH_OK || cnt == 0u) return 0;
    uint8_t hdr[8];
    if (tq->nv_ops->read_at(tq->nv_ops->ctx, 0u, 0u, hdr, sizeof(hdr)) != HONCH_OK) return 0;
    if (get_u64le(hdr) != sequence) return 0;
    return tq->nv_ops->consume_front(tq->nv_ops->ctx, 1u) == HONCH_OK ? 1 : 0;
}

static honch_status_t tq_consume(void *ctx, uint64_t sequence) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (tq_nv_consume_if_head(tq, sequence)) return HONCH_OK;
    return tq->ram_ops.queue_consume(tq->ram_ops.ctx, sequence);
}

static honch_status_t tq_consume_batch(void *ctx, const uint64_t *sequences, size_t sequence_count) {
    if (sequences == NULL && sequence_count > 0u) return HONCH_ERROR_INVALID_ARGUMENT;
    honch_status_t status = HONCH_OK;
    for (size_t i = 0; status == HONCH_OK && i < sequence_count; i++) {
        status = tq_consume(ctx, sequences[i]);
    }
    return status;
}

static honch_status_t tq_dead_letter(void *ctx, uint64_t sequence) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (tq_nv_consume_if_head(tq, sequence)) return HONCH_OK; /* drop from NV */
    return tq->ram_ops.queue_dead_letter(tq->ram_ops.ctx, sequence);
}

static honch_status_t tq_dead_letter_batch(void *ctx, const uint64_t *sequences, size_t sequence_count) {
    if (sequences == NULL && sequence_count > 0u) return HONCH_ERROR_INVALID_ARGUMENT;
    honch_status_t status = HONCH_OK;
    for (size_t i = 0; status == HONCH_OK && i < sequence_count; i++) {
        status = tq_dead_letter(ctx, sequences[i]);
    }
    return status;
}

static honch_status_t tq_drop_oldest(void *ctx) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK && cnt > 0u) {
            return tq->nv_ops->consume_front(tq->nv_ops->ctx, 1u);
        }
    }
    return tq->ram_ops.queue_drop_oldest(tq->ram_ops.ctx);
}

static honch_status_t tq_clear(void *ctx) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    honch_status_t status = tq->ram_ops.queue_clear(tq->ram_ops.ctx);
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK && cnt > 0u) {
            tq->nv_ops->consume_front(tq->nv_ops->ctx, cnt);
        }
    }
    tq->peek_cursor = 0u;
    return status;
}

static honch_status_t tq_depth(void *ctx, size_t *depth) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (depth == NULL) return HONCH_ERROR_INVALID_ARGUMENT;
    size_t ram_depth = 0;
    honch_status_t status = tq->ram_ops.queue_depth(tq->ram_ops.ctx, &ram_depth);
    if (status != HONCH_OK) return status;
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK) ram_depth += cnt;
    }
    *depth = ram_depth;
    return HONCH_OK;
}

static honch_status_t tq_get_stats(void *ctx, honch_queue_stats_t *stats) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    honch_status_t status = tq->ram_ops.queue_get_stats(tq->ram_ops.ctx, stats);
    if (status != HONCH_OK) return status;
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK) stats->queued_events += cnt;
        if (tq->nv_ops->get_stats != NULL) {
            honch_nv_queue_stats_t nvs = {0, 0, 0, 0};
            if (tq->nv_ops->get_stats(tq->nv_ops->ctx, &nvs) == HONCH_OK) {
                stats->queued_bytes += nvs.stored_bytes;
            }
        }
    }
    return HONCH_OK;
}

/* peek iterates the NV tier (oldest-first) so the core can recover the highest
 * persisted sequence at startup; RAM is empty at boot so it is delegated last. */
static honch_status_t tq_peek(void *ctx, honch_storage_reader_t *reader) {
    honch_tiered_queue_t *tq = (honch_tiered_queue_t *)ctx;
    if (reader == NULL) return HONCH_ERROR_INVALID_ARGUMENT;
    if (tq_nv_active(tq)) {
        size_t cnt = 0;
        if (tq_nv_count(tq, &cnt) == HONCH_OK && tq->peek_cursor < cnt) {
            uint8_t hdr[HONCH_NV_REC_HEADER];
            if (tq->nv_ops->read_at(tq->nv_ops->ctx, tq->peek_cursor, 0u, hdr, sizeof(hdr)) == HONCH_OK) {
                *reader = (honch_storage_reader_t){
                    .ctx = NULL,
                    .read = NULL,
                    .total_size = get_u32le(hdr + 8),
                    .sequence = get_u64le(hdr),
                };
                tq->peek_cursor++;
                return HONCH_OK;
            }
        }
        tq->peek_cursor = 0u;
    }
    return tq->ram_ops.queue_peek(tq->ram_ops.ctx, reader);
}

honch_status_t honch_tiered_queue_init(
    honch_tiered_queue_t *tq,
    const honch_event_queue_ops_t *ram_ops,
    const honch_nv_queue_ops_t *nv_ops,
    const honch_tiered_queue_config_t *config,
    uint8_t *read_scratch,
    size_t read_scratch_size) {
    if (tq == NULL || ram_ops == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    tq->ram_ops = *ram_ops;
    tq->nv_ops = nv_ops;
    tq->config = config != NULL ? *config : (honch_tiered_queue_config_t){0, 0};
    tq->read_scratch = read_scratch;
    tq->read_scratch_size = read_scratch_size;
    tq->peek_cursor = 0u;
    return HONCH_OK;
}

honch_status_t honch_tiered_queue_ops_init(
    honch_event_queue_ops_t *ops,
    honch_tiered_queue_t *tq) {
    if (ops == NULL || tq == NULL) {
        return HONCH_ERROR_INVALID_ARGUMENT;
    }
    ops->queue_push = tq_push;
    ops->queue_peek = tq_peek;
    ops->queue_read_batch = tq_read_batch;
    ops->queue_consume = tq_consume;
    ops->queue_consume_batch = tq_consume_batch;
    ops->queue_dead_letter = tq_dead_letter;
    ops->queue_dead_letter_batch = tq_dead_letter_batch;
    ops->queue_drop_oldest = tq_drop_oldest;
    ops->queue_clear = tq_clear;
    ops->queue_depth = tq_depth;
    ops->queue_get_stats = tq_get_stats;
    ops->ctx = tq;
    return HONCH_OK;
}
