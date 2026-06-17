set(HONCH_ESP_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(HONCH_SDK_ROOT "${HONCH_ESP_COMPONENT_DIR}/../../..")

set(HONCH_ESP_PORT_SOURCES
    "src/esp_compat.c"
    "src/esp_core_shims.c"
    "src/esp_platform.c"
    "src/esp_retry_after.c"
    "src/esp_storage.c"
    "src/esp_transport_http.c"
)
list(TRANSFORM HONCH_ESP_PORT_SOURCES PREPEND "${HONCH_ESP_COMPONENT_DIR}/")

set(HONCH_CORE_SOURCES
    "core/src/honch_capture_transport.c"
    "core/src/honch_core.c"
    "core/src/honch_event_record.c"
    "core/src/honch_lifecycle.c"
    "core/src/honch_queue_policy.c"
    "core/src/honch_ram_queue.c"
    "core/src/honch_support.c"
    "core/src/honch_wire_v2.c"
)
list(TRANSFORM HONCH_CORE_SOURCES PREPEND "${HONCH_SDK_ROOT}/")

set(HONCH_ESP_REQUIRES
    esp_app_format
    esp_http_client
    esp-tls
    esp_timer
    efuse
    freertos
)

if(CONFIG_HONCH_CRASH_SYMBOLICATION)
    list(APPEND HONCH_ESP_REQUIRES espcoredump)
endif()

idf_component_register(
    SRCS
        ${HONCH_ESP_PORT_SOURCES}
        ${HONCH_CORE_SOURCES}
    INCLUDE_DIRS
        "${HONCH_ESP_COMPONENT_DIR}/include"
        "${HONCH_SDK_ROOT}/core/include"
    PRIV_INCLUDE_DIRS
        "${HONCH_ESP_COMPONENT_DIR}/src"
        "${HONCH_SDK_ROOT}/core/src"
    REQUIRES
        ${HONCH_ESP_REQUIRES}
)

if(CONFIG_HONCH_ERROR_TRACKING)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_ERROR_TRACKING=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_ERROR_TRACKING=0)
endif()

if(CONFIG_HONCH_CRASH_SYMBOLICATION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_ENABLE_CRASH_SYMBOLICATION=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_ENABLE_CRASH_SYMBOLICATION=0)
endif()

if(CONFIG_RATE_SWEEP_FLUSH_TIMING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_FLUSH_TIMING=1)
endif()
