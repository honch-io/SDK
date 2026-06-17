#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

extern "C" {
#include "honch/core/ram_queue.h"
#include "honch_nv_queue.h"
#include "honch_tiered_queue.h"
}

// ---------------------------------------------------------------------------
// Fake in-RAM non-volatile adapter: an oldest-first FIFO of opaque blobs.
// ---------------------------------------------------------------------------
struct FakeNv {
  std::vector<std::vector<uint8_t>> blobs;  // index 0 == oldest
  bool on = true;
  bool fail_append = false;  // simulate an NV tier that is full / erroring
  uint64_t dropped = 0;
};

static int fake_enabled(void *ctx) { return ((FakeNv *)ctx)->on ? 1 : 0; }

static honch_status_t fake_count(void *ctx, size_t *out) {
  *out = ((FakeNv *)ctx)->blobs.size();
  return HONCH_OK;
}

static honch_status_t fake_length_at(void *ctx, size_t index, size_t *out) {
  FakeNv *nv = (FakeNv *)ctx;
  if (index >= nv->blobs.size()) return HONCH_ERROR_INVALID_ARGUMENT;
  *out = nv->blobs[index].size();
  return HONCH_OK;
}

static honch_status_t fake_read_at(void *ctx, size_t index, uint32_t offset, uint8_t *buf, size_t n) {
  FakeNv *nv = (FakeNv *)ctx;
  if (index >= nv->blobs.size()) return HONCH_ERROR_INVALID_ARGUMENT;
  std::vector<uint8_t> &b = nv->blobs[index];
  if ((size_t)offset > b.size() || n > b.size() - (size_t)offset) return HONCH_ERROR_INVALID_ARGUMENT;
  memcpy(buf, b.data() + offset, n);
  return HONCH_OK;
}

static honch_status_t fake_append(void *ctx, honch_nv_read_cb reader, void *reader_ctx, size_t total_size) {
  FakeNv *nv = (FakeNv *)ctx;
  if (nv->fail_append) return HONCH_ERROR_IO;
  std::vector<uint8_t> blob(total_size);
  // Pull in two chunks to exercise the reader's offset handling.
  size_t half = total_size / 2;
  if (half > 0) {
    honch_status_t s = reader(reader_ctx, 0, blob.data(), half);
    if (s != HONCH_OK) return s;
  }
  honch_status_t s = reader(reader_ctx, (uint32_t)half, blob.data() + half, total_size - half);
  if (s != HONCH_OK) return s;
  nv->blobs.push_back(std::move(blob));
  return HONCH_OK;
}

static honch_status_t fake_consume_front(void *ctx, size_t count) {
  FakeNv *nv = (FakeNv *)ctx;
  if (count > nv->blobs.size()) return HONCH_ERROR_INVALID_ARGUMENT;
  nv->blobs.erase(nv->blobs.begin(), nv->blobs.begin() + count);
  return HONCH_OK;
}

static honch_nv_queue_ops_t fake_ops(FakeNv *nv) {
  honch_nv_queue_ops_t ops = {};
  ops.enabled = fake_enabled;
  ops.count = fake_count;
  ops.length_at = fake_length_at;
  ops.read_at = fake_read_at;
  ops.append = fake_append;
  ops.consume_front = fake_consume_front;
  ops.get_stats = NULL;
  ops.ctx = nv;
  return ops;
}

// Test helpers --------------------------------------------------------------
struct Harness {
  uint8_t ram_buf[4096];
  uint8_t scratch[2048];
  honch_ram_queue_t ram;
  honch_event_queue_ops_t ram_ops;
  honch_tiered_queue_t tq;
  honch_event_queue_ops_t ops;
};

static void harness_init(Harness *h, const honch_nv_queue_ops_t *nv,
                         const honch_tiered_queue_config_t *cfg) {
  assert(honch_ram_queue_init(&h->ram, h->ram_buf, sizeof(h->ram_buf)) == HONCH_OK);
  assert(honch_ram_queue_ops_init(&h->ram_ops, &h->ram) == HONCH_OK);
  assert(honch_tiered_queue_init(&h->tq, &h->ram_ops, nv, cfg, h->scratch, sizeof(h->scratch)) == HONCH_OK);
  assert(honch_tiered_queue_ops_init(&h->ops, &h->tq) == HONCH_OK);
}

static honch_status_t push(Harness *h, uint64_t seq, uint8_t tag, size_t len) {
  uint8_t ev[1024];
  assert(len <= sizeof(ev));
  memset(ev, tag, len);
  return h->ops.queue_push(h->ops.ctx, ev, len, seq);
}

static size_t depth(Harness *h) {
  size_t d = 0;
  assert(h->ops.queue_depth(h->ops.ctx, &d) == HONCH_OK);
  return d;
}

// T1: with no NV adapter, behaves exactly like the RAM queue.
static void test_ram_only_passthrough() {
  Harness h;
  harness_init(&h, /*nv=*/NULL, /*cfg=*/NULL);
  assert(push(&h, 1, 0xAA, 4) == HONCH_OK);
  assert(depth(&h) == 1);
  printf("  T1 ram-only passthrough OK\n");
}

// T2: persist spills RAM->NV; depth counts both tiers; bytes round-trip.
static void test_persist_roundtrip() {
  FakeNv nv;
  Harness h;
  honch_tiered_queue_config_t cfg = {0, 0};  // no auto-spill
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  assert(push(&h, 1, 0x11, 8) == HONCH_OK);
  assert(push(&h, 2, 0x22, 8) == HONCH_OK);
  assert(push(&h, 3, 0x33, 8) == HONCH_OK);
  assert(depth(&h) == 3);

  assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);  // drain all RAM->NV
  assert(nv.blobs.size() == 3);
  assert(depth(&h) == 3);  // still 3, now in NV

  // read_batch returns oldest (seq 1) first, byte-for-byte, BORROWED.
  honch_storage_event_t ev[8] = {};
  size_t n = 0;
  assert(h.ops.queue_read_batch(h.ops.ctx, ev, 8, 4096, &n) == HONCH_OK);
  assert(n == 3);
  assert(ev[0].sequence == 1);
  assert(ev[0].length == 8);
  assert((ev[0].flags & HONCH_STORAGE_EVENT_BORROWED) != 0);
  for (size_t i = 0; i < 8; i++) assert(ev[0].data[i] == 0x11);
  assert(ev[2].sequence == 3);
  printf("  T2 persist round-trip OK\n");
}

// T3: pushing past the high-water mark auto-spills to NV.
static void test_pressure_autospill() {
  FakeNv nv;
  Harness h;
  honch_tiered_queue_config_t cfg = {64, 16};  // high=64B, low=16B
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  for (uint64_t i = 1; i <= 10; i++) assert(push(&h, i, (uint8_t)i, 16) == HONCH_OK);
  // 10 * 16 = 160B pushed; RAM should have spilled down toward low-water.
  honch_queue_stats_t st = {};
  assert(h.ram_ops.queue_get_stats(h.ram_ops.ctx, &st) == HONCH_OK);
  assert(st.queued_bytes <= 64);
  assert(nv.blobs.size() > 0);
  assert(depth(&h) == 10);  // nothing lost
  printf("  T3 pressure auto-spill OK (ram_bytes=%zu, nv=%zu)\n", st.queued_bytes, nv.blobs.size());
}

// T4: drain order is NV (older) before RAM (newer); consume routes per-tier.
static void test_drain_order_and_consume() {
  FakeNv nv;
  Harness h;
  honch_tiered_queue_config_t cfg = {0, 0};
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  assert(push(&h, 1, 0x01, 8) == HONCH_OK);
  assert(push(&h, 2, 0x02, 8) == HONCH_OK);
  assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);  // 1,2 -> NV
  assert(push(&h, 3, 0x03, 8) == HONCH_OK);                  // 3,4 stay in RAM
  assert(push(&h, 4, 0x04, 8) == HONCH_OK);
  assert(depth(&h) == 4);

  honch_storage_event_t ev[8] = {};
  size_t n = 0;
  // First drain: NV events 1,2.
  assert(h.ops.queue_read_batch(h.ops.ctx, ev, 8, 4096, &n) == HONCH_OK);
  assert(n == 2);
  assert(ev[0].sequence == 1 && ev[1].sequence == 2);
  uint64_t seqs[2] = {1, 2};
  assert(h.ops.queue_consume_batch(h.ops.ctx, seqs, 2) == HONCH_OK);
  assert(nv.blobs.size() == 0);
  assert(depth(&h) == 2);

  // Second drain: RAM events 3,4.
  memset(ev, 0, sizeof(ev));
  assert(h.ops.queue_read_batch(h.ops.ctx, ev, 8, 4096, &n) == HONCH_OK);
  assert(n == 2);
  assert(ev[0].sequence == 3 && ev[1].sequence == 4);
  uint64_t seqs2[2] = {3, 4};
  assert(h.ops.queue_consume_batch(h.ops.ctx, seqs2, 2) == HONCH_OK);
  assert(depth(&h) == 0);
  printf("  T4 drain order + consume routing OK\n");
}

// T5: after a "reboot", peek recovers NV sequences (max) for the core.
static void test_boot_recovery_peek() {
  FakeNv nv;
  {
    Harness h;
    honch_tiered_queue_config_t cfg = {0, 0};
    honch_nv_queue_ops_t ops = fake_ops(&nv);
    harness_init(&h, &ops, &cfg);
    assert(push(&h, 10, 0xA0, 8) == HONCH_OK);
    assert(push(&h, 11, 0xA1, 8) == HONCH_OK);
    assert(push(&h, 12, 0xA2, 8) == HONCH_OK);
    assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);
    assert(nv.blobs.size() == 3);
  }
  // Reboot: fresh RAM + tiered queue over the same (persisted) NV.
  Harness h2;
  honch_tiered_queue_config_t cfg = {0, 0};
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h2, &ops, &cfg);
  assert(depth(&h2) == 3);

  uint64_t max_seq = 0;
  for (int i = 0; i < 3; i++) {
    honch_storage_reader_t r = {};
    assert(h2.ops.queue_peek(h2.ops.ctx, &r) == HONCH_OK);
    if (r.sequence > max_seq) max_seq = r.sequence;
  }
  assert(max_seq == 12);
  honch_storage_reader_t r = {};
  assert(h2.ops.queue_peek(h2.ops.ctx, &r) == HONCH_ERROR_NOT_INITIALIZED);
  printf("  T5 boot recovery via peek OK (max_seq=%llu)\n", (unsigned long long)max_seq);
}

// T6a: a corrupt (bad-CRC) NV record is dropped on read; the queue survives.
static void test_torn_record_dropped() {
  FakeNv nv;
  Harness h;
  honch_tiered_queue_config_t cfg = {0, 0};
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  assert(push(&h, 5, 0x55, 8) == HONCH_OK);
  assert(push(&h, 6, 0x66, 8) == HONCH_OK);
  assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);
  assert(nv.blobs.size() == 2);

  // Corrupt the oldest record's payload (simulate a torn write).
  nv.blobs[0][13] ^= 0xFF;

  honch_storage_event_t ev[8] = {};
  size_t n = 0;
  assert(h.ops.queue_read_batch(h.ops.ctx, ev, 8, 4096, &n) == HONCH_OK);
  // Corrupt seq-5 record dropped; valid seq-6 returned.
  assert(n == 1);
  assert(ev[0].sequence == 6);
  printf("  T6a torn record dropped OK\n");
}

// T6c: a corrupt record carrying a garbage (huge) sequence is skipped by peek,
// so startup recovery never adopts the bad sequence -- peek validates the whole
// record's CRC, not just the header it reads the seq from.
static void test_peek_skips_corrupt_sequence() {
  FakeNv nv;
  {
    Harness h;
    honch_tiered_queue_config_t cfg = {0, 0};
    honch_nv_queue_ops_t ops = fake_ops(&nv);
    harness_init(&h, &ops, &cfg);
    assert(push(&h, 10, 0xA0, 8) == HONCH_OK);
    assert(push(&h, 11, 0xA1, 8) == HONCH_OK);
    assert(push(&h, 12, 0xA2, 8) == HONCH_OK);
    assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);
    assert(nv.blobs.size() == 3);
  }
  // Corrupt the middle record's sequence field to UINT64_MAX: rewrites the seq
  // the header carries and breaks the record's CRC (a torn record with a garbage
  // sequence).
  for (int i = 0; i < 8; i++) nv.blobs[1][i] = 0xFF;

  Harness h2;
  honch_tiered_queue_config_t cfg = {0, 0};
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h2, &ops, &cfg);

  uint64_t max_seq = 0;
  for (int i = 0; i < 8; i++) {
    honch_storage_reader_t r = {};
    if (h2.ops.queue_peek(h2.ops.ctx, &r) != HONCH_OK) break;
    if (r.sequence > max_seq) max_seq = r.sequence;
  }
  // The corrupt record's UINT64_MAX seq must NOT be adopted; max stays 12.
  assert(max_seq == 12);
  printf("  T6c peek skips corrupt sequence OK (max_seq=%llu)\n", (unsigned long long)max_seq);
}

// T6b: when the adapter reports disabled, the queue stays RAM-only.
static void test_disabled_adapter_ram_only() {
  FakeNv nv;
  nv.on = false;
  Harness h;
  honch_tiered_queue_config_t cfg = {16, 0};  // would auto-spill if enabled
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  for (uint64_t i = 1; i <= 5; i++) assert(push(&h, i, (uint8_t)i, 16) == HONCH_OK);
  assert(honch_tiered_queue_persist(&h.tq, 0) == HONCH_OK);  // no-op while disabled
  assert(nv.blobs.size() == 0);
  assert(depth(&h) == 5);  // all still in RAM
  printf("  T6b disabled adapter stays RAM-only OK\n");
}

// T7: persist must surface a real error when the NV tier fails an append,
// rather than reporting success while events are left unpersisted. A graceful-
// shutdown flush that returns HONCH_OK is taken to mean "events are durable";
// swallowing the failure silently loses them at power-off.
static void test_persist_propagates_nv_error() {
  FakeNv nv;
  nv.fail_append = true;  // NV enabled but every append errors (e.g. flash full)
  Harness h;
  honch_tiered_queue_config_t cfg = {0, 0};
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  assert(push(&h, 1, 0x11, 8) == HONCH_OK);
  assert(push(&h, 2, 0x22, 8) == HONCH_OK);

  honch_status_t status = honch_tiered_queue_persist(&h.tq, 0);
  assert(status != HONCH_OK);   // must NOT claim success
  assert(nv.blobs.size() == 0);  // nothing actually persisted
  printf("  T7 persist propagates NV error OK\n");
}

// T8: under RAM pressure with an NV tier available, the oldest events must spill
// to durable NV instead of being silently dropped from RAM -- even with auto-
// spill disabled (the default config). This is the core durability/ordering
// contract: nothing is lost while a tier still has room.
static void test_push_spills_instead_of_dropping() {
  FakeNv nv;
  Harness h;
  honch_tiered_queue_config_t cfg = {0, 0};  // default: auto-spill OFF
  honch_nv_queue_ops_t ops = fake_ops(&nv);
  harness_init(&h, &ops, &cfg);

  // RAM buffer is 4096B; 512B events => 8 fit. Push 12 to force back-pressure.
  const uint64_t kCount = 12;
  for (uint64_t i = 1; i <= kCount; i++) assert(push(&h, i, (uint8_t)i, 512) == HONCH_OK);

  honch_queue_stats_t st = {};
  assert(h.ram_ops.queue_get_stats(h.ram_ops.ctx, &st) == HONCH_OK);
  assert(st.capacity_dropped_events == 0);  // nothing dropped from RAM
  assert(nv.blobs.size() > 0);              // overflow went to durable NV
  assert(depth(&h) == kCount);              // every event retained

  // Drain order must still be oldest-first across tiers (NV holds the oldest).
  honch_storage_event_t ev[16] = {};
  size_t n = 0;
  assert(h.ops.queue_read_batch(h.ops.ctx, ev, 16, 4096, &n) == HONCH_OK);
  assert(n > 0);
  assert(ev[0].sequence == 1);  // oldest event survived, first out
  printf("  T8 push spills to NV instead of dropping OK (nv=%zu, depth=%zu)\n",
         nv.blobs.size(), depth(&h));
}

int main() {
  test_ram_only_passthrough();
  test_persist_roundtrip();
  test_pressure_autospill();
  test_drain_order_and_consume();
  test_boot_recovery_peek();
  test_torn_record_dropped();
  test_peek_skips_corrupt_sequence();
  test_disabled_adapter_ram_only();
  test_persist_propagates_nv_error();
  test_push_spills_instead_of_dropping();
  printf("ALL TIERED QUEUE TESTS PASSED\n");
  return 0;
}
