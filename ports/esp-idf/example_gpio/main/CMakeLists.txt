idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        SDK
        esp_driver_gpio
        esp_event
        esp_netif
        esp_wifi
        freertos
        nvs_flash
)
