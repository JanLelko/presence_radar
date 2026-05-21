idf_component_register(
    SRCS
        "main.c"
        "wifi_mqtt.c"
        "radar.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mqtt
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
)
