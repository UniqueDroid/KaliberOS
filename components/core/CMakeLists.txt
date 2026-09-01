idf_component_register(
    SRCS "event_bus.c" "power_mgr.c" "app_store.c"
    INCLUDE_DIRS "include"
    REQUIRES hal esp_timer
    PRIV_REQUIRES esp_littlefs json
)
