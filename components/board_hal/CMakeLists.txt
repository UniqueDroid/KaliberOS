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
    # esp_lcd_sh8601/esp_lcd_panel_io_additions: waveshare_c6_amoled's
    # display driver only (see that board's board.c) - unused, harmless
    # REQUIRES for every other board, same as any dependency only one
    # board's board.c actually calls into.
    PRIV_REQUIRES core esp_lcd waveshare__esp_lcd_sh8601 espressif__esp_lcd_panel_io_additions
)
