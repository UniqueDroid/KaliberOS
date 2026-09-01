# The board is a plain subdirectory choice: exactly one board.c is built.
if(NOT DEFINED KALIBER_BOARD)
    set(KALIBER_BOARD "watchy_v3")
endif()

idf_component_register(
    SRCS "boards/${KALIBER_BOARD}/board.c"
    INCLUDE_DIRS "include" "boards/${KALIBER_BOARD}"
    # ESP-IDF 6.x split the old monolithic "driver" component into
    # per-peripheral esp_driver_* components.
    REQUIRES esp_driver_gpio esp_driver_spi esp_adc esp_timer freertos
    PRIV_REQUIRES core
)
