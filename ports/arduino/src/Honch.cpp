#include "Honch.h"

#include "honch_arduino_adapter.h"
extern "C" {
#include "honch_internal.h"
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

HonchClass Honch;

namespace {

struct ArduinoQueueEntry {
  uint64_t sequence;
  std::vector<uint8_t> data;
};

struct ArduinoAdapterState {
  std::map<std::string, std::vector<uint8_t>> state;
  std::vector<ArduinoQueueEntry> queue;
  size_t peekIndex = 0;
};

ArduinoAdapterState g_adapterState;

uint64_t arduino_now_ms(void *) {
  return 1700000000000ULL;
}

honch_status_t arduino_random_bytes(void *, uint8_t *buffer, size_t bufferSize) {
  if (buffer == nullptr && bufferSize > 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < bufferSize; ++i) {
    buffer[i] = static_cast<uint8_t>(i & 0xffu);
  }
  return HONCH_OK;
}

void arduino_log(void *, honch_log_level_t, const char *) {}

honch_status_t arduino_state_get(void *ctx, const char *key, uint8_t *buffer, size_t *bufferSize) {
  if (ctx == nullptr || key == nullptr || bufferSize == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  std::map<std::string, std::vector<uint8_t>>::const_iterator found = adapter->state.find(key);
  if (found == adapter->state.end()) {
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
}

honch_status_t arduino_state_set(void *ctx, const char *key, const uint8_t *data, size_t dataSize) {
  if (ctx == nullptr || key == nullptr || (data == nullptr && dataSize > 0)) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  if (dataSize == 0) {
    adapter->state[key] = std::vector<uint8_t>();
  } else {
    adapter->state[key] = std::vector<uint8_t>(data, data + dataSize);
  }
  return HONCH_OK;
}

honch_status_t arduino_state_delete(void *ctx, const char *key) {
  if (ctx == nullptr || key == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  adapter->state.erase(key);
  return HONCH_OK;
}

honch_status_t arduino_queue_push(void *ctx, const uint8_t *event, size_t eventSize, uint64_t sequence) {
  if (ctx == nullptr || (event == nullptr && eventSize > 0)) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  ArduinoQueueEntry entry;
  entry.sequence = sequence;
  entry.data.assign(event, event + eventSize);
  adapter->queue.push_back(entry);
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
  if (ctx == nullptr || reader == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  if (adapter->peekIndex >= adapter->queue.size()) {
    adapter->peekIndex = 0;
    return HONCH_ERROR_NOT_INITIALIZED;
  }

  ArduinoQueueEntry &entry = adapter->queue[adapter->peekIndex++];
  *reader = honch_storage_reader_t{
      &entry,
      arduino_reader_read,
      entry.data.size(),
      entry.sequence,
  };
  return HONCH_OK;
}

honch_status_t arduino_queue_consume(void *ctx, uint64_t sequence) {
  if (ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  for (std::vector<ArduinoQueueEntry>::iterator it = adapter->queue.begin(); it != adapter->queue.end(); ++it) {
    if (it->sequence == sequence) {
      adapter->queue.erase(it);
      adapter->peekIndex = 0;
      return HONCH_OK;
    }
  }
  return HONCH_ERROR_NOT_INITIALIZED;
}

honch_status_t arduino_queue_dead_letter(void *ctx, uint64_t sequence) {
  return arduino_queue_consume(ctx, sequence);
}

honch_status_t arduino_queue_drop_oldest(void *ctx) {
  if (ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  if (!adapter->queue.empty()) {
    adapter->queue.erase(adapter->queue.begin());
    adapter->peekIndex = 0;
  }
  return HONCH_OK;
}

honch_status_t arduino_queue_clear(void *ctx) {
  if (ctx == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  adapter->queue.clear();
  adapter->peekIndex = 0;
  return HONCH_OK;
}

honch_status_t arduino_queue_depth(void *ctx, size_t *depth) {
  if (ctx == nullptr || depth == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  ArduinoAdapterState *adapter = static_cast<ArduinoAdapterState *>(ctx);
  adapter->peekIndex = 0;
  *depth = adapter->queue.size();
  return HONCH_OK;
}

honch_status_t arduino_post_chunk(
    void *,
    const char *,
    const char *,
    const char *,
    const uint8_t *body,
    size_t bodySize,
    honch_transport_result_t *result) {
  if (body == nullptr || bodySize == 0 || result == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  *result = HONCH_TRANSPORT_ACCEPTED;
  return HONCH_OK;
}

const honch_platform_ops_t kPlatformOps = {
    arduino_now_ms,
    arduino_now_ms,
    arduino_random_bytes,
    nullptr,
    nullptr,
    arduino_log,
    nullptr,
};

const honch_storage_ops_t kStorageOps = {
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
    &g_adapterState,
};

const honch_transport_ops_t kTransportOps = {
    arduino_post_chunk,
    nullptr,
};

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

extern "C" {

uint64_t honch_now_millis(void) {
  return arduino_now_ms(nullptr);
}

honch_status_t honch_random_hex(char out[33]) {
  if (out == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  uint8_t bytes[16];
  honch_status_t status = arduino_random_bytes(nullptr, bytes, sizeof(bytes));
  if (status != HONCH_OK) {
    return status;
  }

  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
    out[(i * 2) + 1] = hex[bytes[i] & 0x0f];
  }
  out[32] = '\0';
  return HONCH_OK;
}

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

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config) {
  honch_core_config_t coreConfig = {};
  coreConfig.api_key = config.apiKey;
  coreConfig.endpoint_url = config.host;
  coreConfig.device_id = "host-arduino-device";
  coreConfig.device_model = config.deviceModel;
  coreConfig.firmware_version = config.firmwareVersion;
  coreConfig.environment = config.environment;
  coreConfig.sdk_platform = "arduino-esp32";
  coreConfig.queue_directory = "arduino";
  coreConfig.batch_size = config.flushEventThreshold;
  coreConfig.max_queued_events = 1000;
  coreConfig.max_event_bytes = config.eventBufferSize;
  coreConfig.flush_interval_seconds = config.flushIntervalSeconds;
  coreConfig.flush_event_threshold = config.flushEventThreshold;
  coreConfig.disable_background_flush = 1;
  coreConfig.durability_mode = HONCH_DURABILITY_SYNC_ALWAYS;
  coreConfig.platform = &kPlatformOps;
  coreConfig.storage = &kStorageOps;
  coreConfig.transport = &kTransportOps;
  return coreConfig;
}

void honch_arduino_release_core_config(honch_core_config_t *) {}

HonchClass::HonchClass() : _client(nullptr), _lastStatus(HONCH_ERROR_NOT_INITIALIZED) {}

bool HonchClass::setLastStatus(honch_status_t status) {
  _lastStatus = status;
  return status == HONCH_OK;
}

bool HonchClass::begin(const HonchConfig &config) {
  if (_client != nullptr) {
    return setLastStatus(HONCH_ERROR_ALREADY_INITIALIZED);
  }

  honch_core_config_t coreConfig = honch_arduino_make_core_config(config);
  honch_status_t status = honch_core_init(&_client, &coreConfig);
  honch_arduino_release_core_config(&coreConfig);
  return setLastStatus(status);
}

bool HonchClass::track(const char *eventName, const char *propertiesJson) {
  return setLastStatus(honch_core_track(_client, eventName, propertiesJson == nullptr ? "{}" : propertiesJson));
}

bool HonchClass::identify(const char *distinctId, const char *traitsJson) {
  return setLastStatus(honch_core_identify(_client, distinctId, traitsJson == nullptr ? "{}" : traitsJson));
}

bool HonchClass::setProperty(const char *key, const char *valueJson) {
  return setLastStatus(honch_core_set_property(_client, key, valueJson == nullptr ? "null" : valueJson));
}

bool HonchClass::sessionStart(const char *sessionName) {
  return setLastStatus(honch_core_session_start(_client, sessionName));
}

bool HonchClass::sessionEnd() {
  return setLastStatus(honch_core_session_end(_client));
}

bool HonchClass::flush() {
  return setLastStatus(honch_core_flush(_client));
}

bool HonchClass::shutdown() {
  honch_status_t status = honch_core_shutdown(_client);
  if (status == HONCH_OK) {
    _client = nullptr;
  }
  return setLastStatus(status);
}

bool HonchClass::reset() {
  return setLastStatus(honch_core_reset(_client));
}

const char *HonchClass::deviceId() {
  const char *id = honch_core_get_device_id(_client);
  return id == nullptr ? "" : id;
}

const char *HonchClass::lastError() {
  return honch_status_string(_lastStatus);
}
