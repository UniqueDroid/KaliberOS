cmake_minimum_required(VERSION 3.16)

# Board selection: idf.py -DKALIBER_BOARD=watchy_v3 build
if(NOT DEFINED KALIBER_BOARD)
    set(KALIBER_BOARD "watchy_v3")
endif()
message(STATUS "Kaliber board: ${KALIBER_BOARD}")

# Merge board sdkconfig on top of project defaults
set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults;components/board_hal/boards/${KALIBER_BOARD}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kaliber)
