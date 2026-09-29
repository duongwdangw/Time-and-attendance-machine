idf_component_register(
    SRCS
        "main.c"
        "wifi_time.c"
        "google_sheet.c"

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
        esp_http_client
        esp-tls
        mbedtls
)