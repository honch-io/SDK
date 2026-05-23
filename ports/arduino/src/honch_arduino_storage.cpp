#include "honch_arduino_adapter.h"

extern "C" {
#include "honch_internal.h"
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace {

struct ArduinoQueueEntry {
  uint64_t sequence;
  std::vector<uint8_t> data;
};

struct HostStorageState {
  std::map<std::string, std::vector<uint8_t>> state;
  std::vector<ArduinoQueueEntry> queue;
  size_t peekIndex = 0;
};

HostStorageState g_hostStorage;

#ifdef ARDUINO
Preferences g_preferences;
static const char *HONCH_ARDUINO_STATE_NAMESPACE = "honch";
static const char *HONCH_ARDUINO_QUEUE_NAMESPACE = "honch_q";
static const size_t HONCH_ARDUINO_QUEUE_MAX_DEPTH = 256;
#endif

#ifdef ARDUINO
void arduino_sequence_key(uint64_t sequence, char key[16]) {
  static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char digits[14];
  size_t count = 0;
  do {
    digits[count++] = alphabet[sequence % 36u];
    sequence /= 36u;
  } while (sequence > 0 && count < sizeof(digits));

  key[0] = 'q';
  for (size_t i = 0; i < count; ++i) {
    key[i + 1] = digits[count - i - 1];
  }
  key[count + 1] = '\0';
}

bool arduino_queue_blob_exists(Preferences &preferences, uint64_t sequence, size_t *valueSize) {
  char key[16];
  arduino_sequence_key(sequence, key);
  size_t size = preferences.getBytesLength(key);
  if (size == 0) {
    return false;
  }
  if (valueSize != nullptr) {
    *valueSize = size;
  }
  return true;
}

honch_status_t arduino_queue_advance_tail(Preferences &preferences, uint64_t head, uint64_t tail) {
  while (tail < head && !arduino_queue_blob_exists(preferences, tail, nullptr)) {
    tail++;
  }
  return preferences.putULong64("tail", tail) == sizeof(uint64_t) ? HONCH_OK : HONCH_ERROR_IO;
}
#endif

honch_status_t arduino_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *bufferSize) {
  (void)ctx;
  if (key == nullptr || bufferSize == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  if (!g_preferences.begin(HONCH_ARDUINO_STATE_NAMESPACE, true)) {
    return HONCH_ERROR_IO;
  }
  size_t valueSize = g_preferences.getBytesLength(key);
  if (valueSize == 0) {
    *bufferSize = 0;
    g_preferences.end();
    return HONCH_OK;
  }
  if (buffer == nullptr || *bufferSize < valueSize) {
    *bufferSize = valueSize;
    g_preferences.end();
    return buffer == nullptr ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
  }
  size_t readSize = g_preferences.getBytes(key, buffer, valueSize);
  g_preferences.end();
  if (readSize != valueSize) {
    return HONCH_ERROR_IO;
  }
  *bufferSize = valueSize;
  return HONCH_OK;
#else
  HostStorageState *storage = &g_hostStorage;
  std::map<std::string, std::vector<uint8_t>>::const_iterator found = storage->state.find(key);
  if (found == storage->state.end()) {
    *bufferSize = 0;
    return HONCH_OK;
  }

  const std::vector<uint8_t> &value = found->second;
  if (buffer == nullptr || *bufferSize < value.size()) {
    *bufferSize = value.size();
    return buffer == nullptr ? HONCH_OK : HONCH_ERROR_INVALID_ARGUMENT;
  }

  memcpy(buffer, value.data(), value.size());
  *bufferSize = value.size();
  return HONCH_OK;
#endif
}

honch_status_t arduino_state_set(void *ctx, const char *key, const uint8_t *data, size_t dataSize) {
  (void)ctx;
  if (key == nullptr || (data == nullptr && dataSize > 0)) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  if (!g_preferences.begin(HONCH_ARDUINO_STATE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }
  size_t written = g_preferences.putBytes(key, data, dataSize);
  g_preferences.end();
  return written == dataSize ? HONCH_OK : HONCH_ERROR_IO;
#else
  if (dataSize == 0) {
    g_hostStorage.state[key] = std::vector<uint8_t>();
  } else {
    g_hostStorage.state[key] = std::vector<uint8_t>(data, data + dataSize);
  }
  return HONCH_OK;
#endif
}

honch_status_t arduino_state_delete(void *ctx, const char *key) {
  (void)ctx;
  if (key == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  if (!g_preferences.begin(HONCH_ARDUINO_STATE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }
  g_preferences.remove(key);
  g_preferences.end();
#else
  g_hostStorage.state.erase(key);
#endif
  return HONCH_OK;
}

honch_status_t arduino_queue_push(void *ctx, const uint8_t *event, size_t eventSize, uint64_t sequence) {
  if (event == nullptr || eventSize == 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  (void)ctx;
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }

  uint64_t head = g_preferences.getULong64("head", 0);
  uint64_t tail = g_preferences.getULong64("tail", 0);
  while (head >= tail && head - tail >= HONCH_ARDUINO_QUEUE_MAX_DEPTH) {
    char dropKey[16];
    arduino_sequence_key(tail, dropKey);
    g_preferences.remove(dropKey);
    tail++;
  }

  uint64_t storedSequence = sequence < head ? head : sequence;
  char key[16];
  arduino_sequence_key(storedSequence, key);
  size_t written = g_preferences.putBytes(key, event, eventSize);
  if (written == eventSize) {
    head = storedSequence + 1;
    if (g_preferences.putULong64("head", head) != sizeof(uint64_t)) {
      written = 0;
    }
  }
  if (written == eventSize) {
    (void)arduino_queue_advance_tail(g_preferences, head, tail);
  }
  g_preferences.end();
  return written == eventSize ? HONCH_OK : HONCH_ERROR_IO;
#else
  (void)ctx;
  ArduinoQueueEntry entry;
  entry.sequence = sequence;
  entry.data.assign(event, event + eventSize);
  g_hostStorage.queue.push_back(entry);
  return HONCH_OK;
#endif
}

honch_status_t arduino_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t bufferSize) {
#ifdef ARDUINO
  honch_arduino_storage_t *storage = static_cast<honch_arduino_storage_t *>(ctx);
  if (storage == nullptr || buffer == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, true)) {
    return HONCH_ERROR_IO;
  }
  char key[16];
  arduino_sequence_key(storage->readSequence, key);
  size_t valueSize = g_preferences.getBytesLength(key);
  if (offset > valueSize || bufferSize > valueSize - offset) {
    g_preferences.end();
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  uint8_t *scratch = static_cast<uint8_t *>(malloc(valueSize));
  if (scratch == nullptr) {
    g_preferences.end();
    return HONCH_ERROR_OUT_OF_MEMORY;
  }
  size_t readSize = g_preferences.getBytes(key, scratch, valueSize);
  g_preferences.end();
  if (readSize != valueSize) {
    free(scratch);
    return HONCH_ERROR_IO;
  }
  memcpy(buffer, scratch + offset, bufferSize);
  free(scratch);
  return HONCH_OK;
#else
  ArduinoQueueEntry *entry = static_cast<ArduinoQueueEntry *>(ctx);
  if (entry == nullptr || buffer == nullptr || offset > entry->data.size() ||
      bufferSize > entry->data.size() - offset) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  memcpy(buffer, entry->data.data() + offset, bufferSize);
  return HONCH_OK;
#endif
}

honch_status_t arduino_queue_peek(void *ctx, honch_storage_reader_t *reader) {
  if (reader == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  honch_arduino_storage_t *storage = static_cast<honch_arduino_storage_t *>(ctx);
  if (storage == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, true)) {
    return HONCH_ERROR_NOT_INITIALIZED;
  }
  uint64_t head = g_preferences.getULong64("head", 0);
  uint64_t tail = g_preferences.getULong64("tail", 0);
  uint64_t start = storage->peekSequence == UINT64_MAX ? tail : storage->peekSequence + 1;
  for (uint64_t sequence = start; sequence < head; ++sequence) {
    size_t valueSize = 0;
    if (!arduino_queue_blob_exists(g_preferences, sequence, &valueSize)) {
      continue;
    }
    storage->peekSequence = sequence;
    storage->readSequence = sequence;
    *reader = honch_storage_reader_t{
        storage,
        arduino_reader_read,
        valueSize,
        sequence,
    };
    g_preferences.end();
    return HONCH_OK;
  }
  g_preferences.end();
  return HONCH_ERROR_NOT_INITIALIZED;
#else
  (void)ctx;
  if (g_hostStorage.peekIndex >= g_hostStorage.queue.size()) {
    g_hostStorage.peekIndex = 0;
    return HONCH_ERROR_NOT_INITIALIZED;
  }

  ArduinoQueueEntry &entry = g_hostStorage.queue[g_hostStorage.peekIndex++];
  *reader = honch_storage_reader_t{
      &entry,
      arduino_reader_read,
      entry.data.size(),
      entry.sequence,
  };
  return HONCH_OK;
#endif
}

honch_status_t arduino_queue_consume(void *ctx, uint64_t sequence) {
#ifdef ARDUINO
  (void)ctx;
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }
  uint64_t head = g_preferences.getULong64("head", 0);
  uint64_t tail = g_preferences.getULong64("tail", 0);
  char key[16];
  arduino_sequence_key(sequence, key);
  g_preferences.remove(key);
  if (sequence == tail) {
    tail++;
  }
  honch_status_t status = arduino_queue_advance_tail(g_preferences, head, tail);
  g_preferences.end();
  return status;
#else
  (void)ctx;
  for (std::vector<ArduinoQueueEntry>::iterator it = g_hostStorage.queue.begin(); it != g_hostStorage.queue.end(); ++it) {
    if (it->sequence == sequence) {
      g_hostStorage.queue.erase(it);
      g_hostStorage.peekIndex = 0;
      return HONCH_OK;
    }
  }
  return HONCH_ERROR_NOT_INITIALIZED;
#endif
}

honch_status_t arduino_queue_dead_letter(void *ctx, uint64_t sequence) {
  return arduino_queue_consume(ctx, sequence);
}

honch_status_t arduino_queue_drop_oldest(void *ctx) {
#ifdef ARDUINO
  (void)ctx;
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }
  uint64_t head = g_preferences.getULong64("head", 0);
  uint64_t tail = g_preferences.getULong64("tail", 0);
  if (tail < head) {
    char key[16];
    arduino_sequence_key(tail, key);
    g_preferences.remove(key);
    tail++;
  }
  honch_status_t status = arduino_queue_advance_tail(g_preferences, head, tail);
  g_preferences.end();
  return status;
#else
  (void)ctx;
  if (!g_hostStorage.queue.empty()) {
    g_hostStorage.queue.erase(g_hostStorage.queue.begin());
    g_hostStorage.peekIndex = 0;
  }
  return HONCH_OK;
#endif
}

honch_status_t arduino_queue_clear(void *ctx) {
#ifdef ARDUINO
  honch_arduino_storage_t *storage = static_cast<honch_arduino_storage_t *>(ctx);
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, false)) {
    return HONCH_ERROR_IO;
  }
  g_preferences.clear();
  g_preferences.end();
  if (storage != nullptr) {
    storage->peekSequence = UINT64_MAX;
    storage->readSequence = UINT64_MAX;
  }
  return HONCH_OK;
#else
  (void)ctx;
  g_hostStorage.queue.clear();
  g_hostStorage.peekIndex = 0;
  return HONCH_OK;
#endif
}

honch_status_t arduino_queue_depth(void *ctx, size_t *depth) {
  if (depth == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  honch_arduino_storage_t *storage = static_cast<honch_arduino_storage_t *>(ctx);
  if (storage != nullptr) {
    storage->peekSequence = UINT64_MAX;
    storage->readSequence = UINT64_MAX;
  }
  if (!g_preferences.begin(HONCH_ARDUINO_QUEUE_NAMESPACE, true)) {
    *depth = 0;
    return HONCH_OK;
  }
  uint64_t head = g_preferences.getULong64("head", 0);
  uint64_t tail = g_preferences.getULong64("tail", 0);
  *depth = 0;
  for (uint64_t sequence = tail; sequence < head; ++sequence) {
    if (arduino_queue_blob_exists(g_preferences, sequence, nullptr)) {
      (*depth)++;
    }
  }
  g_preferences.end();
  return HONCH_OK;
#else
  (void)ctx;
  g_hostStorage.peekIndex = 0;
  *depth = g_hostStorage.queue.size();
  return HONCH_OK;
#endif
}

honch_status_t arduino_state_get_string(honch_client_t *client, const char *key, char **out) {
  *out = nullptr;
  if (client == nullptr || client->storage == nullptr || client->storage->state_get == nullptr || key == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  size_t valueSize = 0;
  honch_status_t status = client->storage->state_get(client->storage->ctx, key, nullptr, &valueSize);
  if (status != HONCH_OK || valueSize == 0) {
    return status;
  }

  char *value = static_cast<char *>(malloc(valueSize + 1));
  if (value == nullptr) {
    return HONCH_ERROR_OUT_OF_MEMORY;
  }

  size_t readSize = valueSize;
  status = client->storage->state_get(client->storage->ctx, key, reinterpret_cast<uint8_t *>(value), &readSize);
  if (status != HONCH_OK) {
    free(value);
    return status;
  }

  value[readSize] = '\0';
  *out = value;
  return HONCH_OK;
}

honch_status_t arduino_write_state_string(honch_client_t *client, const char *key, const char *value) {
  if (client == nullptr || client->storage == nullptr || client->storage->state_set == nullptr ||
      key == nullptr || value == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  return client->storage->state_set(
      client->storage->ctx,
      key,
      reinterpret_cast<const uint8_t *>(value),
      strlen(value));
}

honch_status_t arduino_copy_or_null(char **out, const char *value) {
  *out = nullptr;
  if (value == nullptr) {
    return HONCH_OK;
  }

  *out = honch_strdup(value);
  return *out == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
}

} // namespace

honch_status_t honch_arduino_storage_ops_init(
    honch_storage_ops_t *ops,
    honch_arduino_storage_t *ctx,
    const HonchConfig &config) {
  if (ops == nullptr || ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ctx->eventBuffer = config.eventBuffer;
  ctx->eventBufferSize = config.eventBufferSize;
  ctx->peekSequence = UINT64_MAX;
  ctx->readSequence = UINT64_MAX;
  *ops = honch_storage_ops_t{
      .state_get = arduino_state_get,
      .state_set = arduino_state_set,
      .state_delete = arduino_state_delete,
      .queue_push = arduino_queue_push,
      .queue_peek = arduino_queue_peek,
      .queue_read_batch = nullptr,
      .queue_consume = arduino_queue_consume,
      .queue_consume_batch = nullptr,
      .queue_dead_letter = arduino_queue_dead_letter,
      .queue_dead_letter_batch = nullptr,
      .queue_drop_oldest = arduino_queue_drop_oldest,
      .queue_clear = arduino_queue_clear,
      .queue_depth = arduino_queue_depth,
      .ctx = ctx,
  };
  return HONCH_OK;
}

extern "C" {

honch_status_t honch_state_prepare(honch_client_t *client, const honch_core_config_t *config) {
  if (client == nullptr || config == nullptr || client->storage == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  honch_status_t status = HONCH_OK;
  char *storedDeviceId = nullptr;
  if (config->device_id != nullptr && !honch_is_blank(config->device_id)) {
    client->configured_device_id = true;
    client->device_id = honch_strdup(config->device_id);
    if (client->device_id == nullptr) {
      return HONCH_ERROR_OUT_OF_MEMORY;
    }
    status = arduino_write_state_string(client, "device_id", client->device_id);
  } else {
    status = arduino_state_get_string(client, "device_id", &storedDeviceId);
    if (status == HONCH_OK && !honch_is_blank(storedDeviceId)) {
      client->device_id = storedDeviceId;
      storedDeviceId = nullptr;
    } else if (status == HONCH_OK) {
      char generated[33];
      status = honch_random_hex(generated);
      if (status == HONCH_OK) {
        client->device_id = honch_strdup(generated);
        if (client->device_id == nullptr) {
          status = HONCH_ERROR_OUT_OF_MEMORY;
        }
      }
      if (status == HONCH_OK) {
        status = arduino_write_state_string(client, "device_id", client->device_id);
      }
    }
  }
  free(storedDeviceId);
  if (status != HONCH_OK) {
    return status;
  }

  char *storedDistinctId = nullptr;
  status = arduino_state_get_string(client, "distinct_id", &storedDistinctId);
  if (status != HONCH_OK) {
    return status;
  }
  if (!honch_is_blank(storedDistinctId)) {
    client->distinct_id = storedDistinctId;
    storedDistinctId = nullptr;
  } else {
    client->distinct_id = honch_strdup(client->device_id);
    if (client->distinct_id == nullptr) {
      status = HONCH_ERROR_OUT_OF_MEMORY;
    } else {
      status = honch_state_save_distinct_id(client);
    }
  }
  free(storedDistinctId);
  if (status != HONCH_OK) {
    return status;
  }

  status = arduino_copy_or_null(&client->device_model, config->device_model);
  if (status == HONCH_OK) {
    status = arduino_copy_or_null(&client->firmware_version, config->firmware_version);
  }
  if (status == HONCH_OK && !honch_is_blank(config->environment)) {
    status = arduino_copy_or_null(&client->environment, config->environment);
  } else if (status == HONCH_OK) {
    client->environment = honch_strdup("production");
    if (client->environment == nullptr) {
      status = HONCH_ERROR_OUT_OF_MEMORY;
    }
  }

  return status;
}

honch_status_t honch_state_save_distinct_id(honch_client_t *client) {
  return honch_state_save_distinct_id_value(client, client->distinct_id);
}

honch_status_t honch_state_save_distinct_id_value(honch_client_t *client, const char *distinctId) {
  return arduino_write_state_string(client, "distinct_id", distinctId);
}

honch_status_t honch_state_check_firmware_version(
    honch_client_t *client,
    bool *changed,
    char **previousVersion) {
  if (client == nullptr || changed == nullptr || previousVersion == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  *changed = false;
  *previousVersion = nullptr;
  if (honch_is_blank(client->firmware_version)) {
    return HONCH_OK;
  }

  char *storedVersion = nullptr;
  honch_status_t status = arduino_state_get_string(client, "firmware_version", &storedVersion);
  if (status != HONCH_OK) {
    return status;
  }

  if (!honch_is_blank(storedVersion) && strcmp(storedVersion, client->firmware_version) != 0) {
    *previousVersion = storedVersion;
    storedVersion = nullptr;
    *changed = true;
  }

  status = arduino_write_state_string(client, "firmware_version", client->firmware_version);
  if (status != HONCH_OK) {
    free(*previousVersion);
    *previousVersion = nullptr;
    *changed = false;
  }

  free(storedVersion);
  return status;
}

honch_status_t honch_state_reset(honch_client_t *client) {
  if (client == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  honch_status_t status = HONCH_OK;
  char *deviceId = nullptr;
  char *distinctId = nullptr;
  if (client->configured_device_id) {
    deviceId = honch_strdup(client->device_id);
    distinctId = honch_strdup(client->device_id);
  } else {
    char nextDeviceId[33];
    status = honch_random_hex(nextDeviceId);
    if (status == HONCH_OK) {
      deviceId = honch_strdup(nextDeviceId);
      distinctId = honch_strdup(nextDeviceId);
    }
  }
  if (status == HONCH_OK && (deviceId == nullptr || distinctId == nullptr)) {
    status = HONCH_ERROR_OUT_OF_MEMORY;
  }

  if (status == HONCH_OK) {
    status = arduino_write_state_string(client, "device_id", deviceId);
  }
  if (status == HONCH_OK) {
    status = arduino_write_state_string(client, "distinct_id", distinctId);
  }
  if (status == HONCH_OK) {
    free(client->device_id);
    free(client->distinct_id);
    client->device_id = deviceId;
    client->distinct_id = distinctId;
    deviceId = nullptr;
    distinctId = nullptr;
  }

  free(deviceId);
  free(distinctId);
  return status;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t eventSize) {
  if (client == nullptr || client->storage == nullptr || client->storage->queue_push == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  return client->storage->queue_push(client->storage->ctx, event, eventSize, client->sequence++);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count) {
  if (client == nullptr || client->storage == nullptr || client->storage->queue_depth == nullptr || count == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  return client->storage->queue_depth(client->storage->ctx, count);
}

honch_status_t honch_queue_clear(honch_client_t *client) {
  if (client == nullptr || client->storage == nullptr || client->storage->queue_clear == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  return client->storage->queue_clear(client->storage->ctx);
}

}
