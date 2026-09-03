# Engine backend is selected by the board capability at compile time via
# KALIBER_ENGINE (quickjs | mquickjs). Default: quickjs.
if(NOT DEFINED KALIBER_ENGINE)
    set(KALIBER_ENGINE "quickjs")
endif()

if(KALIBER_ENGINE STREQUAL "quickjs")
    set(ENGINE_SRC "engine_quickjs.c")
else()
    set(ENGINE_SRC "engine_mqjs.c")   # TODO: MQuickJS backend
endif()

idf_component_register(
    SRCS "${ENGINE_SRC}" "modules/js_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal core
    PRIV_REQUIRES quickjs gfx   # provide as managed component or components/quickjs
)
