set(HONCH_ESP_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(HONCH_SDK_ROOT "${HONCH_ESP_COMPONENT_DIR}/../../..")

# Core sources live at <SDK_ROOT>/core in the monorepo. When this component is
# packed for the Espressif Component Registry, the registry archive is rooted at
# this directory and cannot reach outside it, so the publish step vendors a copy
# to <component>/core. Prefer that local copy when present; fall back to the
# monorepo layout for in-tree (Citadel / examples) builds.
if(EXISTS "${HONCH_ESP_COMPONENT_DIR}/core/src/honch_core.c")
    set(HONCH_CORE_ROOT "${HONCH_ESP_COMPONENT_DIR}")
else()
    set(HONCH_CORE_ROOT "${HONCH_SDK_ROOT}")
endif()

set(HONCH_ESP_PORT_SOURCES
    "src/esp_compat.c"
    "src/esp_core_shims.c"
    "src/esp_http_status.c"
    "src/esp_platform.c"
    "src/esp_retry_after.c"
    "src/esp_storage.c"
    "src/esp_transport_http.c"
)
list(TRANSFORM HONCH_ESP_PORT_SOURCES PREPEND "${HONCH_ESP_COMPONENT_DIR}/")

set(HONCH_CORE_SOURCES
    "core/src/honch_battery.c"
    "core/src/honch_capture_transport.c"
    "core/src/honch_core.c"
    "core/src/honch_coredump.c"
    "core/src/honch_crash.c"
    "core/src/honch_diagnostics.c"
    "core/src/honch_error_detail.c"
    "core/src/honch_event_record.c"
    "core/src/honch_lifecycle.c"
    "core/src/honch_lifecycle_events.c"
    "core/src/honch_queue_policy.c"
    "core/src/honch_ram_queue.c"
    "core/src/honch_sessions.c"
    "core/src/honch_support.c"
    "core/src/honch_wire_v2.c"
)
list(TRANSFORM HONCH_CORE_SOURCES PREPEND "${HONCH_CORE_ROOT}/")

# espcoredump (image/summary API) + esp_partition (stream the raw coredump image
# off the coredump partition) MUST be unconditional requirements: component
# requirements are resolved in IDF's early-expansion pass before sdkconfig is
# available, so gating them on CONFIG_HONCH_CRASH_SYMBOLICATION silently drops the
# dependency and the build fails once the coredump code is compiled in. With crash
# capture disabled at build time both components contribute essentially nothing to
# the image.
set(HONCH_ESP_REQUIRES
    esp_app_format
    esp_http_client
    esp-tls
    esp_timer
    efuse
    freertos
    espcoredump
    esp_partition
)

idf_component_register(
    SRCS
        ${HONCH_ESP_PORT_SOURCES}
        ${HONCH_CORE_SOURCES}
    INCLUDE_DIRS
        "${HONCH_ESP_COMPONENT_DIR}/include"
        "${HONCH_CORE_ROOT}/core/include"
    PRIV_INCLUDE_DIRS
        "${HONCH_ESP_COMPONENT_DIR}/src"
        "${HONCH_CORE_ROOT}/core/src"
    REQUIRES
        ${HONCH_ESP_REQUIRES}
)

if(CONFIG_HONCH_ERROR_TRACKING)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_ERROR_TRACKING=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_ERROR_TRACKING=0)
endif()

if(CONFIG_HONCH_LIFECYCLE_EVENTS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_LIFECYCLE_EVENTS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_LIFECYCLE_EVENTS=0)
endif()

if(CONFIG_HONCH_SESSIONS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_SESSIONS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_SESSIONS=0)
endif()

if(CONFIG_HONCH_BATTERY)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_BATTERY=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HONCH_ENABLE_BATTERY=0)
endif()

if(CONFIG_HONCH_CRASH_SYMBOLICATION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_ENABLE_CRASH_SYMBOLICATION=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_ENABLE_CRASH_SYMBOLICATION=0)
endif()

if(CONFIG_RATE_SWEEP_FLUSH_TIMING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HONCH_FLUSH_TIMING=1)
endif()
