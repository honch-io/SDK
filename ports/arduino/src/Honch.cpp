#include "Honch.h"

#include "honch_arduino_adapter.h"

namespace {

HonchClass g_defaultClient;
honch_arduino_platform_t g_platform;
honch_arduino_storage_t g_storage;
honch_arduino_transport_t g_transport;
honch_platform_ops_t g_platformOps;
honch_event_queue_ops_t g_eventQueueOps;
honch_transport_ops_t g_transportOps;
#if HONCH_ENABLE_ERROR_TRACKING
honch_crash_report_t gCrashReport;
#endif
honch_status_t gConfigStatus = HONCH_OK;
bool (*gConnectivityCallback)() = nullptr;

int honch_arduino_connectivity_callback(void *) {
  return gConnectivityCallback == nullptr || gConnectivityCallback() ? 1 : 0;
}

} // namespace

namespace honch {

HonchClass &defaultClient() {
  return g_defaultClient;
}

} // namespace honch

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config) {
  honch_core_config_t coreConfig = {};
  gConfigStatus = HONCH_OK;
  gConnectivityCallback = config.connectivityCallback;

  gConfigStatus = honch_arduino_platform_ops_init(&g_platformOps, &g_platform);
  if (config.eventQueueOps != nullptr) {
    g_eventQueueOps = *config.eventQueueOps;
  } else if (gConfigStatus == HONCH_OK) {
    gConfigStatus = honch_arduino_storage_ops_init(&g_eventQueueOps, &g_storage, config);
  }
  if (gConfigStatus == HONCH_OK) {
    gConfigStatus = honch_arduino_transport_ops_init(&g_transportOps, &g_transport, config);
  }
#if HONCH_ENABLE_ERROR_TRACKING
  gCrashReport = honch_arduino_crash_report();
#endif

  coreConfig.api_key = config.apiKey;
  coreConfig.endpoint_url = config.host;
  coreConfig.device_id = config.deviceId;
  coreConfig.device_model = config.deviceModel;
  coreConfig.firmware_version = config.firmwareVersion;
  coreConfig.environment = config.environment;
  coreConfig.sdk_platform = "arduino-esp32";
  coreConfig.queue_directory = "";
  coreConfig.batch_size = config.flushEventThreshold;
  // Soft ceiling only: the tiered RAM+NV queue's real capacity is bounded by the
  // event buffer and the NV tier, which may be well below this. The core treats
  // it as an upper bound on the queued count, not a guarantee the storage tier
  // can hold that many; drop-oldest still kicks in when the tiers actually fill.
  coreConfig.max_queued_events = 1000;
  coreConfig.max_event_bytes = config.eventBufferSize;
  coreConfig.transport_timeout_ms = config.transportTimeoutMs;
  coreConfig.flush_interval_seconds = config.flushIntervalSeconds;
  coreConfig.flush_min_interval_ms = config.flushMinIntervalMs;
  coreConfig.flush_event_threshold = config.flushEventThreshold;
  coreConfig.connectivity_callback = honch_arduino_connectivity_callback;
  coreConfig.connectivity_userdata = nullptr;
#if HONCH_ENABLE_ERROR_TRACKING
  coreConfig.crash_report = config.enableErrorTracking ? &gCrashReport : nullptr;
#else
  coreConfig.crash_report = nullptr;
#endif
  coreConfig.platform = &g_platformOps;
  coreConfig.state_storage = config.stateStorageOps;
  coreConfig.event_queue = &g_eventQueueOps;
  coreConfig.transport = &g_transportOps;
  return coreConfig;
}

void honch_arduino_release_core_config(honch_core_config_t *) {}

HonchClass::HonchClass()
    : _client(nullptr),
      _instanceMutex(nullptr),
      _lastStatus(HONCH_ERROR_NOT_INITIALIZED) {
  honch_status_t status = honch_arduino_mutex_create(nullptr, &_instanceMutex);
  if (status != HONCH_OK) {
    _lastStatus = status;
  }
}

HonchClass::~HonchClass() {
  if (_instanceMutex != nullptr) {
    honch_arduino_mutex_destroy(nullptr, _instanceMutex);
    _instanceMutex = nullptr;
  }
}

honch_status_t HonchClass::lockInstance() {
  if (_instanceMutex == nullptr) {
    honch_status_t status = honch_arduino_mutex_create(nullptr, &_instanceMutex);
    if (status != HONCH_OK) {
      _lastStatus.store(status);
      return status;
    }
  }
  honch_status_t status = honch_arduino_mutex_lock(nullptr, _instanceMutex);
  if (status != HONCH_OK) {
    _lastStatus.store(status);
  }
  return status;
}

void HonchClass::unlockInstance() {
  honch_arduino_mutex_unlock(nullptr, _instanceMutex);
}

bool HonchClass::setLastStatusLocked(honch_status_t status) {
  _lastStatus.store(status);
  return status == HONCH_OK;
}

bool HonchClass::begin(const HonchConfig &config) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }

  if (_client != nullptr) {
    bool ok = setLastStatusLocked(HONCH_ERROR_ALREADY_INITIALIZED);
    unlockInstance();
    return ok;
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
  bool ok = setLastStatusLocked(status);
  unlockInstance();
  return ok;
}

bool HonchClass::track(const char *eventName, const honch_property_t *properties, size_t propertyCount) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_track(_client, eventName, properties, propertyCount));
  unlockInstance();
  return ok;
}

bool HonchClass::identify(const char *distinctId, const honch_property_t *traits, size_t traitCount) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_identify(_client, distinctId, traits, traitCount));
  unlockInstance();
  return ok;
}

bool HonchClass::setProperty(const char *key, honch_value_t value) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_set_property(_client, key, value));
  unlockInstance();
  return ok;
}

bool HonchClass::sessionStart(const char *sessionName) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_session_start(_client, sessionName));
  unlockInstance();
  return ok;
}

bool HonchClass::sessionEnd() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_session_end(_client));
  unlockInstance();
  return ok;
}

bool HonchClass::flush() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  honch_status_t status = honch_core_flush(_client);
  bool ok = setLastStatusLocked(status);
  unlockInstance();
  return ok;
}

bool HonchClass::tick() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  if (_client == nullptr) {
    bool ok = setLastStatusLocked(HONCH_ERROR_NOT_INITIALIZED);
    unlockInstance();
    return ok;
  }

  honch_status_t status = honch_core_tick(_client);
  bool ok = setLastStatusLocked(status);
  unlockInstance();
  return ok;
}

bool HonchClass::loop() {
  return tick();
}

bool HonchClass::shutdown() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  if (_client == nullptr) {
    bool ok = setLastStatusLocked(HONCH_ERROR_NOT_INITIALIZED);
    unlockInstance();
    return ok;
  }

  honch_client_t *client = _client;
  honch_status_t status = honch_core_shutdown(client);
  if (status != HONCH_ERROR_BUSY) {
    _client = nullptr;
    honch_arduino_storage_ops_deinit(&g_storage);
  }
  bool ok = setLastStatusLocked(status);
  unlockInstance();
  return ok;
}

bool HonchClass::reset() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_reset(_client));
  unlockInstance();
  return ok;
}

const char *HonchClass::deviceId() {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return "";
  }
  const char *id = honch_core_get_device_id(_client);
  const char *result = id == nullptr ? "" : id;
  unlockInstance();
  return result;
}

bool HonchClass::queueStats(honch_queue_stats_t *stats) {
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  bool ok = setLastStatusLocked(honch_core_get_queue_stats(_client, stats));
  unlockInstance();
  return ok;
}

const char *HonchClass::lastError() {
  honch_status_t status = _lastStatus.load();
  honch_status_t lockStatus = lockInstance();
  if (lockStatus == HONCH_OK) {
    status = _lastStatus.load();
    unlockInstance();
  } else {
    status = lockStatus;
  }
  return honch_status_string(status);
}

bool HonchClass::lastErrorDetail(honch_error_detail_t *out) {
  if (out == nullptr) {
    return false;
  }
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return false;
  }
  honch_status_t status = _client == nullptr
                              ? HONCH_ERROR_NOT_INITIALIZED
                              : honch_core_get_last_error(_client, out);
  unlockInstance();
  return status == HONCH_OK;
}

const char *HonchClass::lastErrorMessage() {
  honch_error_detail_t detail = {};
  if (!lastErrorDetail(&detail)) {
    // Fall back to the coarse string if detail is unavailable.
    return lastError();
  }
  honch_status_t lockStatus = lockInstance();
  if (lockStatus != HONCH_OK) {
    return honch_status_string(detail.status);
  }
  honch_error_detail_format(&detail, _lastErrorMessage, sizeof(_lastErrorMessage));
  unlockInstance();
  return _lastErrorMessage;
}

#ifndef ARDUINO
bool HonchClass::hostLockForTest() {
  return lockInstance() == HONCH_OK;
}

void HonchClass::hostUnlockForTest() {
  unlockInstance();
}
#endif
