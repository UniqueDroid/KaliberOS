idf_component_register(
    SRCS "event_bus.c" "power_mgr.c" "app_store.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal esp_timer mbedtls
    # Managed components register under namespace__name, not the package
    # name or the header prefix ("esp_littlefs", "cJSON") - see
    # idf_component.yml.
    PRIV_REQUIRES joltwallet__littlefs espressif__cjson nvs_flash
)
