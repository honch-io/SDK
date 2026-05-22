# ESP-IDF component requirements are collected during early expansion before
# this project's -D variant options are available. Keep Honch as an
# unconditional build dependency; FOOTPRINT_ENABLE_HONCH controls references.
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES honch esp_event esp_http_client esp_netif esp_timer esp_wifi heap nvs_flash
)

if(HONCH_FOOTPRINT_WITH_SDK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_ENABLE_HONCH=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_ENABLE_HONCH=0)
endif()

if(HONCH_FOOTPRINT_CONNECTED_BASELINE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_CONNECTED_BASELINE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_CONNECTED_BASELINE=0)
endif()
