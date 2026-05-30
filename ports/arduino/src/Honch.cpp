#include "Honch.h"

#include "honch_arduino_adapter.h"

HonchClass Honch;

namespace {

honch_arduino_platform_t g_platform;
honch_arduino_storage_t g_storage;
honch_arduino_transport_t g_transport;
honch_platform_ops_t g_platformOps;
honch_storage_ops_t g_storageOps;
honch_transport_ops_t g_transportOps;
char g_deviceId[32];

} // namespace

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config) {
  honch_core_config_t coreConfig = {};

  (void)honch_arduino_platform_ops_init(&g_platformOps, &g_platform);
  (void)honch_arduino_storage_ops_init(&g_storageOps, &g_storage, config);
  (void)honch_arduino_transport_ops_init(&g_transportOps, &g_transport, config);

  g_deviceId[0] = '\0';
  (void)honch_arduino_device_id(&g_platform, g_deviceId, sizeof(g_deviceId));

  coreConfig.api_key = config.apiKey;
  coreConfig.endpoint_url = config.host;
  coreConfig.device_id = g_deviceId[0] == '\0' ? nullptr : g_deviceId;
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
  coreConfig.durability_mode = HONCH_DURABILITY_SYNC_ALWAYS;
  coreConfig.platform = &g_platformOps;
  coreConfig.storage = &g_storageOps;
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
  honch_status_t status = honch_core_init(&_client, &coreConfig);
  honch_arduino_release_core_config(&coreConfig);
  if (status != HONCH_OK) {
    _client = nullptr;
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
