#include "honch_nv_littlefs.h"

#include <LittleFS.h>

#include <string.h>

namespace {
constexpr size_t kIdDigits = 10;   // zero-padded record id width
constexpr size_t kIoChunk = 256;   // append copy chunk size
}  // namespace

HonchNvLittleFs::HonchNvLittleFs(const char *dir, size_t max_records, size_t max_bytes)
    : dir_(dir), max_records_(max_records), max_bytes_(max_bytes) {}

void HonchNvLittleFs::recordPath(char *out, size_t out_len, uint64_t id) const {
  // dir + "/" + zero-padded id
  snprintf(out, out_len, "%s/%0*llu", dir_, (int)kIdDigits, (unsigned long long)id);
}

bool HonchNvLittleFs::begin() {
  if (!LittleFS.exists(dir_)) {
    if (!LittleFS.mkdir(dir_)) {
      mounted_ = false;
      return false;
    }
  }

  File root = LittleFS.open(dir_);
  if (!root || !root.isDirectory()) {
    mounted_ = false;
    return false;
  }

  uint64_t min_id = UINT64_MAX;
  uint64_t max_id = 0;
  size_t count = 0;
  size_t bytes = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    const char *name = f.name();
    // f.name() is the bare id on arduino-esp32 3.x but a full path
    // (e.g. "/honch_q/0000000042") on 2.x; strip any leading directory so
    // recovery works on both. The basename is the zero-padded id; ignore
    // anything else (e.g. "tmp").
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    bool numeric = base[0] != '\0';
    for (const char *p = base; *p; p++) {
      if (*p < '0' || *p > '9') { numeric = false; break; }
    }
    if (!numeric) {
      f.close();
      continue;
    }
    uint64_t id = strtoull(base, nullptr, 10);
    if (id < min_id) min_id = id;
    if (id > max_id) max_id = id;
    count++;
    bytes += (size_t)f.size();
    f.close();
  }
  root.close();

  // Clean up any leftover temp file from an interrupted append.
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%s/tmp", dir_);
  if (LittleFS.exists(tmp)) {
    LittleFS.remove(tmp);
  }

  if (count == 0) {
    head_id_ = 1;
    next_id_ = 1;
  } else {
    head_id_ = min_id;
    next_id_ = max_id + 1;
  }
  count_ = count;
  bytes_ = bytes;
  mounted_ = true;
  return true;
}

bool HonchNvLittleFs::dropOldest() {
  if (count_ == 0) return false;
  char path[64];
  recordPath(path, sizeof(path), head_id_);
  size_t sz = 0;
  File f = LittleFS.open(path, "r");
  if (f) {
    sz = (size_t)f.size();
    f.close();
  }
  if (!LittleFS.remove(path)) return false;
  head_id_++;
  count_--;
  bytes_ = (bytes_ >= sz) ? bytes_ - sz : 0;
  return true;
}

honch_nv_queue_ops_t HonchNvLittleFs::ops() {
  honch_nv_queue_ops_t o = {};
  o.enabled = cb_enabled;
  o.count = cb_count;
  o.length_at = cb_length_at;
  o.read_at = cb_read_at;
  o.append = cb_append;
  o.consume_front = cb_consume_front;
  o.get_stats = cb_get_stats;
  o.ctx = this;
  return o;
}

int HonchNvLittleFs::cb_enabled(void *ctx) {
  return static_cast<HonchNvLittleFs *>(ctx)->mounted_ ? 1 : 0;
}

honch_status_t HonchNvLittleFs::cb_count(void *ctx, size_t *out) {
  *out = static_cast<HonchNvLittleFs *>(ctx)->count_;
  return HONCH_OK;
}

honch_status_t HonchNvLittleFs::cb_length_at(void *ctx, size_t index, size_t *out) {
  HonchNvLittleFs *self = static_cast<HonchNvLittleFs *>(ctx);
  if (index >= self->count_) return HONCH_ERROR_INVALID_ARGUMENT;
  char path[64];
  self->recordPath(path, sizeof(path), self->head_id_ + index);
  File f = LittleFS.open(path, "r");
  if (!f) return HONCH_ERROR_IO;
  *out = (size_t)f.size();
  f.close();
  return HONCH_OK;
}

honch_status_t HonchNvLittleFs::cb_read_at(void *ctx, size_t index, uint32_t offset, uint8_t *buf, size_t n) {
  HonchNvLittleFs *self = static_cast<HonchNvLittleFs *>(ctx);
  if (index >= self->count_) return HONCH_ERROR_INVALID_ARGUMENT;
  char path[64];
  self->recordPath(path, sizeof(path), self->head_id_ + index);
  File f = LittleFS.open(path, "r");
  if (!f) return HONCH_ERROR_IO;
  honch_status_t status = HONCH_OK;
  if (!f.seek(offset) || f.read(buf, n) != (int)n) {
    status = HONCH_ERROR_IO;
  }
  f.close();
  return status;
}

honch_status_t HonchNvLittleFs::cb_append(void *ctx, honch_nv_read_cb reader, void *reader_ctx, size_t total_size) {
  HonchNvLittleFs *self = static_cast<HonchNvLittleFs *>(ctx);
  if (!self->mounted_) return HONCH_ERROR_NOT_INITIALIZED;

  // Make room within capacity caps (drop-oldest), keeping space for this record.
  while ((self->max_records_ != 0 && self->count_ + 1 > self->max_records_) ||
         (self->max_bytes_ != 0 && self->bytes_ + total_size > self->max_bytes_)) {
    if (!self->dropOldest()) break;
  }

  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%s/tmp", self->dir_);
  File f = LittleFS.open(tmp, "w");
  if (!f) return HONCH_ERROR_IO;

  uint8_t chunk[kIoChunk];
  size_t written = 0;
  honch_status_t status = HONCH_OK;
  while (written < total_size) {
    size_t want = total_size - written;
    if (want > sizeof(chunk)) want = sizeof(chunk);
    if (reader(reader_ctx, (uint32_t)written, chunk, want) != HONCH_OK) {
      status = HONCH_ERROR_IO;
      break;
    }
    if (f.write(chunk, want) != want) {
      status = HONCH_ERROR_IO;
      break;
    }
    written += want;
  }
  f.close();
  if (status != HONCH_OK) {
    LittleFS.remove(tmp);
    return status;
  }

  char path[64];
  self->recordPath(path, sizeof(path), self->next_id_);
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return HONCH_ERROR_IO;
  }
  self->next_id_++;
  self->count_++;
  self->bytes_ += total_size;
  return HONCH_OK;
}

honch_status_t HonchNvLittleFs::cb_consume_front(void *ctx, size_t n) {
  HonchNvLittleFs *self = static_cast<HonchNvLittleFs *>(ctx);
  if (n > self->count_) return HONCH_ERROR_INVALID_ARGUMENT;
  for (size_t i = 0; i < n; i++) {
    if (!self->dropOldest()) return HONCH_ERROR_IO;
  }
  return HONCH_OK;
}

honch_status_t HonchNvLittleFs::cb_get_stats(void *ctx, honch_nv_queue_stats_t *stats) {
  HonchNvLittleFs *self = static_cast<HonchNvLittleFs *>(ctx);
  stats->stored_events = self->count_;
  stats->stored_bytes = self->bytes_;
  stats->capacity_bytes = self->max_bytes_;
  stats->dropped_events = 0;
  return HONCH_OK;
}
