idf_component_register(
    SRCS "net_svc.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal gfx core
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash
)
