idf_component_register(
    SRCS "event_bus.c" "power_mgr.c" "app_store.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal esp_timer
    # Managed component "joltwallet/littlefs" registers under this name
    # (namespace__name), not its package name or the header prefix
    # "esp_littlefs" - see idf_component.yml.
    # No cJSON dependency yet - app_store.c's manifest parsing is still a
    # TODO, add "json"/a cJSON managed component back when that lands.
    PRIV_REQUIRES joltwallet__littlefs
)
