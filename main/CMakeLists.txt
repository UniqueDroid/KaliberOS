idf_component_register(
    SRCS "main.c"
    REQUIRES board_hal core launcher cadran nvs_flash esp_timer
)
