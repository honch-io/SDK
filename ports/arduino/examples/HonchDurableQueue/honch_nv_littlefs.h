#ifndef HONCH_NV_LITTLEFS_H
#define HONCH_NV_LITTLEFS_H

// Reference non-volatile queue adapter backed by LittleFS, for use as the cold
// tier of honch_tiered_queue. This is example/reference code: copy it into your
// firmware and adapt the directory and capacity to your product. The SDK core
// stays storage-agnostic — this file is the only place LittleFS is named.
//
// Storage model: an oldest-first FIFO where each event record is a single file
// named with a zero-padded monotonic id (e.g. "/honch_q/0000000042"). A new
// record is written to a temp file and renamed into place, so a power loss
// mid-write never corrupts an existing record (and the tiered queue verifies a
// CRC on read as a second line of defence).

#include <Arduino.h>

extern "C" {
#include "honch_nv_queue.h"
}

class HonchNvLittleFs {
 public:
  // dir: directory to hold record files (created if missing).
  // max_records / max_bytes: capacity caps; on overflow the oldest record is
  // dropped to make room (0 = no cap for that dimension).
  HonchNvLittleFs(const char *dir, size_t max_records, size_t max_bytes);

  // Mounts state by scanning `dir`. LittleFS itself must already be begun by
  // the application. Returns false if the directory is unusable.
  bool begin();

  // Fill an ops struct bound to this instance (ops.ctx = this).
  honch_nv_queue_ops_t ops();

 private:
  static int cb_enabled(void *ctx);
  static honch_status_t cb_count(void *ctx, size_t *out);
  static honch_status_t cb_length_at(void *ctx, size_t index, size_t *out);
  static honch_status_t cb_read_at(void *ctx, size_t index, uint32_t offset, uint8_t *buf, size_t n);
  static honch_status_t cb_append(void *ctx, honch_nv_read_cb reader, void *reader_ctx, size_t total_size);
  static honch_status_t cb_consume_front(void *ctx, size_t n);
  static honch_status_t cb_get_stats(void *ctx, honch_nv_queue_stats_t *stats);

  void recordPath(char *out, size_t out_len, uint64_t id) const;
  bool dropOldest();

  const char *dir_;
  size_t max_records_;
  size_t max_bytes_;
  bool mounted_ = false;
  uint64_t head_id_ = 1;   // id of the oldest record
  uint64_t next_id_ = 1;   // id to assign to the next appended record
  size_t count_ = 0;       // number of stored records
  size_t bytes_ = 0;       // total stored bytes (record file sizes)
};

#endif
