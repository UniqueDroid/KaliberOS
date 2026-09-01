# The board is a plain subdirectory choice: exactly one board.c is built.
if(NOT DEFINED KALIBER_BOARD)
    set(KALIBER_BOARD "watchy_v3")
endif()

idf_component_register(
    SRCS "boards/${KALIBER_BOARD}/board.c"
    INCLUDE_DIRS "include" "boards/${KALIBER_BOARD}"
    REQUIRES driver esp_adc esp_timer
    PRIV_REQUIRES core
)
