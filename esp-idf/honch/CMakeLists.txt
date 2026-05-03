idf_component_register(
    SRCS
        "src/honch.c"
        "src/identity.c"
        "src/queue.c"
        "src/encoder.c"
        "src/transport.c"
        "src/scheduler.c"
        "src/gpio.c"
        "src/lifecycle.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        nvs_flash
        esp_http_client
        esp_wifi
        esp_event
        esp-tls
        json
        driver
        efuse
)
