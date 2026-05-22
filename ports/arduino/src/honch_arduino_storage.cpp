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
#endif

honch_status_t arduino_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *bufferSize) {
  (void)ctx;
  if (key == nullptr || bufferSize == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

#ifdef ARDUINO
  if (!g_preferences.begin("honch", true)) {
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
  if (!g_preferences.begin("honch", false)) {
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
  if (!g_preferences.begin("honch", false)) {
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
  (void)ctx;
  if (event == nullptr || eventSize == 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoQueueEntry entry;
  entry.sequence = sequence;
  entry.data.assign(event, event + eventSize);
  g_hostStorage.queue.push_back(entry);
  return HONCH_OK;
}

honch_status_t arduino_reader_read(void *ctx, uint32_t offset, uint8_t *buffer, size_t bufferSize) {
  ArduinoQueueEntry *entry = static_cast<ArduinoQueueEntry *>(ctx);
  if (entry == nullptr || buffer == nullptr || offset > entry->data.size() ||
      bufferSize > entry->data.size() - offset) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  memcpy(buffer, entry->data.data() + offset, bufferSize);
  return HONCH_OK;
}

honch_status_t arduino_queue_peek(void *ctx, honch_storage_reader_t *reader) {
  (void)ctx;
  if (reader == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

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
}

honch_status_t arduino_queue_consume(void *ctx, uint64_t sequence) {
  (void)ctx;
  for (std::vector<ArduinoQueueEntry>::iterator it = g_hostStorage.queue.begin(); it != g_hostStorage.queue.end(); ++it) {
    if (it->sequence == sequence) {
      g_hostStorage.queue.erase(it);
      g_hostStorage.peekIndex = 0;
      return HONCH_OK;
    }
  }
  return HONCH_ERROR_NOT_INITIALIZED;
}

honch_status_t arduino_queue_dead_letter(void *ctx, uint64_t sequence) {
  return arduino_queue_consume(ctx, sequence);
}

honch_status_t arduino_queue_drop_oldest(void *ctx) {
  (void)ctx;
  if (!g_hostStorage.queue.empty()) {
    g_hostStorage.queue.erase(g_hostStorage.queue.begin());
    g_hostStorage.peekIndex = 0;
  }
  return HONCH_OK;
}

honch_status_t arduino_queue_clear(void *ctx) {
  (void)ctx;
  g_hostStorage.queue.clear();
  g_hostStorage.peekIndex = 0;
  return HONCH_OK;
}

honch_status_t arduino_queue_depth(void *ctx, size_t *depth) {
  (void)ctx;
  if (depth == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  g_hostStorage.peekIndex = 0;
  *depth = g_hostStorage.queue.size();
  return HONCH_OK;
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
  *ops = honch_storage_ops_t{
      arduino_state_get,
      arduino_state_set,
      arduino_state_delete,
      arduino_queue_push,
      arduino_queue_peek,
      arduino_queue_consume,
      arduino_queue_dead_letter,
      arduino_queue_drop_oldest,
      arduino_queue_clear,
      arduino_queue_depth,
      ctx,
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
