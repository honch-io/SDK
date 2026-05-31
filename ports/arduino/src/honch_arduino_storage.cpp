#include "honch_arduino_adapter.h"
#include "honch_internal.h"

#include <stdlib.h>
#include <string.h>

namespace {

bool arduino_is_blank(const char *value) {
  if (value == nullptr) {
    return true;
  }
  while (*value != '\0') {
    if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r') {
      return false;
    }
    value++;
  }
  return true;
}

char *arduino_strdup(const char *value) {
  if (value == nullptr) {
    return nullptr;
  }
  size_t length = strlen(value);
  char *copy = static_cast<char *>(malloc(length + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  memcpy(copy, value, length + 1);
  return copy;
}

honch_status_t arduino_copy_or_null(char **out, const char *value) {
  *out = nullptr;
  if (value == nullptr) {
    return HONCH_OK;
  }
  *out = arduino_strdup(value);
  return *out == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
}

bool arduino_has_state_storage(honch_client_t *client) {
  return client != nullptr &&
         client->state_storage != nullptr &&
         client->state_storage->state_get != nullptr &&
         client->state_storage->state_set != nullptr;
}

honch_status_t arduino_state_get_string(honch_client_t *client, const char *key, char **out) {
  *out = nullptr;
  if (!arduino_has_state_storage(client) || key == nullptr) {
    return HONCH_OK;
  }

  size_t valueSize = 0;
  honch_status_t status = client->state_storage->state_get(client->state_storage->ctx, key, nullptr, &valueSize);
  if (status != HONCH_OK || valueSize == 0) {
    return status;
  }

  char *value = static_cast<char *>(malloc(valueSize + 1));
  if (value == nullptr) {
    return HONCH_ERROR_OUT_OF_MEMORY;
  }
  size_t readSize = valueSize;
  status = client->state_storage->state_get(
      client->state_storage->ctx,
      key,
      reinterpret_cast<uint8_t *>(value),
      &readSize);
  if (status != HONCH_OK || readSize > valueSize) {
    free(value);
    return status == HONCH_OK ? HONCH_ERROR_INVALID_ARGUMENT : status;
  }
  value[readSize] = '\0';
  *out = value;
  return HONCH_OK;
}

honch_status_t arduino_write_state_string(honch_client_t *client, const char *key, const char *value) {
  if (key == nullptr || value == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  if (!arduino_has_state_storage(client)) {
    return HONCH_OK;
  }
  return client->state_storage->state_set(
      client->state_storage->ctx,
      key,
      reinterpret_cast<const uint8_t *>(value),
      strlen(value));
}

} // namespace

honch_status_t honch_arduino_storage_ops_init(
    honch_event_queue_ops_t *ops,
    honch_arduino_storage_t *ctx,
    const HonchConfig &config) {
  if (ops == nullptr || ctx == nullptr || config.eventBuffer == nullptr || config.eventBufferSize == 0) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  honch_status_t status = honch_ram_queue_init(&ctx->ramQueue, config.eventBuffer, config.eventBufferSize);
  if (status != HONCH_OK) {
    return status;
  }
  status = honch_ram_queue_ops_init(ops, &ctx->ramQueue);
  if (status != HONCH_OK) {
    honch_ram_queue_deinit(&ctx->ramQueue);
  }
  return status;
}

void honch_arduino_storage_ops_deinit(honch_arduino_storage_t *ctx) {
  if (ctx != nullptr) {
    honch_ram_queue_deinit(&ctx->ramQueue);
  }
}

extern "C" {

honch_status_t honch_state_prepare(honch_client_t *client, const honch_core_config_t *config) {
  if (client == nullptr || config == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }

  honch_status_t status = HONCH_OK;
  if (config->device_id != nullptr && !arduino_is_blank(config->device_id)) {
    client->configured_device_id = true;
    client->device_id = arduino_strdup(config->device_id);
    status = client->device_id == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
  } else {
    char generated[32];
    status = honch_arduino_device_id(nullptr, generated, sizeof(generated));
    if (status == HONCH_OK) {
      client->device_id = arduino_strdup(generated);
      status = client->device_id == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
    }
  }
  if (status != HONCH_OK) {
    return status;
  }

  char *storedDistinctId = nullptr;
  status = arduino_state_get_string(client, "distinct_id", &storedDistinctId);
  if (status != HONCH_OK) {
    return status;
  }
  if (!arduino_is_blank(storedDistinctId)) {
    client->distinct_id = storedDistinctId;
    storedDistinctId = nullptr;
  } else {
    client->distinct_id = arduino_strdup(client->device_id);
    status = client->distinct_id == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
  }
  free(storedDistinctId);
  if (status != HONCH_OK) {
    return status;
  }

  status = arduino_copy_or_null(&client->device_model, config->device_model);
  if (status == HONCH_OK) {
    status = arduino_copy_or_null(&client->firmware_version, config->firmware_version);
  }
  if (status == HONCH_OK && !arduino_is_blank(config->environment)) {
    status = arduino_copy_or_null(&client->environment, config->environment);
  } else if (status == HONCH_OK) {
    client->environment = arduino_strdup("production");
    status = client->environment == nullptr ? HONCH_ERROR_OUT_OF_MEMORY : HONCH_OK;
  }
  return status;
}

honch_status_t honch_state_save_distinct_id(honch_client_t *client) {
  return honch_state_save_distinct_id_value(client, client != nullptr ? client->distinct_id : nullptr);
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
  if (!arduino_has_state_storage(client) || arduino_is_blank(client->firmware_version)) {
    return HONCH_OK;
  }
  char *storedVersion = nullptr;
  honch_status_t status = arduino_state_get_string(client, "firmware_version", &storedVersion);
  if (status != HONCH_OK) {
    return status;
  }
  if (!arduino_is_blank(storedVersion) && strcmp(storedVersion, client->firmware_version) != 0) {
    *previousVersion = storedVersion;
    storedVersion = nullptr;
    *changed = true;
  }
  free(storedVersion);
  return HONCH_OK;
}

honch_status_t honch_state_save_firmware_version(honch_client_t *client) {
  if (client == nullptr || arduino_is_blank(client->firmware_version)) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  return arduino_write_state_string(client, "firmware_version", client->firmware_version);
}

honch_status_t honch_state_reset(honch_client_t *client) {
  if (client == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  char *distinctId = arduino_strdup(client->device_id);
  if (distinctId == nullptr) {
    return HONCH_ERROR_OUT_OF_MEMORY;
  }
  honch_status_t status = honch_state_save_distinct_id_value(client, distinctId);
  if (status != HONCH_OK) {
    free(distinctId);
    return status;
  }
  free(client->distinct_id);
  client->distinct_id = distinctId;
  return HONCH_OK;
}

honch_status_t honch_queue_enqueue(honch_client_t *client, const unsigned char *event, size_t eventSize) {
  if (client == nullptr || client->event_queue == nullptr || client->event_queue->queue_push == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  if (client->sequence == UINT64_MAX) {
    return HONCH_ERROR_QUEUE_FULL;
  }
  return client->event_queue->queue_push(client->event_queue->ctx, event, eventSize, client->sequence++);
}

honch_status_t honch_queue_count_pending(honch_client_t *client, size_t *count) {
  if (client == nullptr || client->event_queue == nullptr || client->event_queue->queue_depth == nullptr || count == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  return client->event_queue->queue_depth(client->event_queue->ctx, count);
}

}

honch_status_t honch_queue_clear(honch_client_t *client) {
  if (client == nullptr || client->event_queue == nullptr || client->event_queue->queue_clear == nullptr) {
    return HONCH_ERROR_INVALID_ARGUMENT;
  }
  return client->event_queue->queue_clear(client->event_queue->ctx);
}
