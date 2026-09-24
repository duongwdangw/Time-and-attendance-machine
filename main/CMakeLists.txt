idf_component_register(
    SRCS
        "main.c"
        "wifi_time.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lwip
        driver
        esp_timer
        esp_lcd
)