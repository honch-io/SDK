#include "Honch.h"

#include "honch_arduino_adapter.h"

HonchClass Honch;

namespace {

honch_arduino_platform_t g_platform;
honch_arduino_storage_t g_storage;
honch_arduino_transport_t g_transport;
honch_platform_ops_t g_platformOps;
honch_event_queue_ops_t g_eventQueueOps;
honch_transport_ops_t g_transportOps;
honch_status_t gConfigStatus = HONCH_OK;

} // namespace

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config) {
  honch_core_config_t coreConfig = {};
  gConfigStatus = HONCH_OK;

  gConfigStatus = honch_arduino_platform_ops_init(&g_platformOps, &g_platform);
  if (config.eventQueueOps != nullptr) {
    g_eventQueueOps = *config.eventQueueOps;
  } else if (gConfigStatus == HONCH_OK) {
    gConfigStatus = honch_arduino_storage_ops_init(&g_eventQueueOps, &g_storage, config);
  }
  if (gConfigStatus == HONCH_OK) {
    gConfigStatus = honch_arduino_transport_ops_init(&g_transportOps, &g_transport, config);
  }

  coreConfig.api_key = config.apiKey;
  coreConfig.endpoint_url = config.host;
  coreConfig.device_id = config.deviceId;
  coreConfig.device_model = config.deviceModel;
  coreConfig.firmware_version = config.firmwareVersion;
  coreConfig.environment = config.environment;
  coreConfig.sdk_platform = "arduino-esp32";
  coreConfig.queue_directory = "";
  coreConfig.batch_size = config.flushEventThreshold;
  coreConfig.max_queued_events = 1000;
  coreConfig.max_event_bytes = config.eventBufferSize;
  coreConfig.flush_interval_seconds = config.flushIntervalSeconds;
  coreConfig.flush_event_threshold = config.flushEventThreshold;
  coreConfig.platform = &g_platformOps;
  coreConfig.state_storage = config.stateStorageOps;
  coreConfig.event_queue = &g_eventQueueOps;
  coreConfig.transport = &g_transportOps;
  return coreConfig;
}

void honch_arduino_release_core_config(honch_core_config_t *) {}

HonchClass::HonchClass()
    : _client(nullptr),
      _lastStatus(HONCH_ERROR_NOT_INITIALIZED) {}

bool HonchClass::setLastStatus(honch_status_t status) {
  _lastStatus = status;
  return status == HONCH_OK;
}

bool HonchClass::recordQueuedStatus(honch_status_t status) {
  return setLastStatus(status);
}

bool HonchClass::begin(const HonchConfig &config) {
  if (_client != nullptr) {
    return setLastStatus(HONCH_ERROR_ALREADY_INITIALIZED);
  }

  honch_core_config_t coreConfig = honch_arduino_make_core_config(config);
  honch_status_t status = gConfigStatus;
  if (status == HONCH_OK) {
    status = honch_core_init(&_client, &coreConfig);
  }
  honch_arduino_release_core_config(&coreConfig);
  if (status != HONCH_OK) {
    _client = nullptr;
    honch_arduino_storage_ops_deinit(&g_storage);
  }
  return setLastStatus(status);
}

bool HonchClass::track(const char *eventName, const honch_property_t *properties, size_t propertyCount) {
  return recordQueuedStatus(honch_core_track(_client, eventName, properties, propertyCount));
}

bool HonchClass::identify(const char *distinctId, const honch_property_t *traits, size_t traitCount) {
  return recordQueuedStatus(honch_core_identify(_client, distinctId, traits, traitCount));
}

bool HonchClass::setProperty(const char *key, honch_value_t value) {
  return recordQueuedStatus(honch_core_set_property(_client, key, value));
}

bool HonchClass::sessionStart(const char *sessionName) {
  return recordQueuedStatus(honch_core_session_start(_client, sessionName));
}

bool HonchClass::sessionEnd() {
  return recordQueuedStatus(honch_core_session_end(_client));
}

bool HonchClass::flush() {
  honch_status_t status = honch_core_flush(_client);
  return setLastStatus(status);
}

bool HonchClass::tick() {
  if (_client == nullptr) {
    return setLastStatus(HONCH_ERROR_NOT_INITIALIZED);
  }

  honch_status_t status = honch_core_tick(_client);
  return setLastStatus(status);
}

bool HonchClass::loop() {
  return tick();
}

bool HonchClass::shutdown() {
  if (_client == nullptr) {
    return setLastStatus(HONCH_ERROR_NOT_INITIALIZED);
  }

  honch_client_t *client = _client;
  honch_status_t status = honch_core_shutdown(client);
  if (status != HONCH_ERROR_BUSY) {
    _client = nullptr;
    honch_arduino_storage_ops_deinit(&g_storage);
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

bool HonchClass::queueStats(honch_queue_stats_t *stats) {
  return setLastStatus(honch_core_get_queue_stats(_client, stats));
}

const char *HonchClass::lastError() {
  return honch_status_string(_lastStatus);
}
