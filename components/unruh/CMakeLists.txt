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
    SRCS "${ENGINE_SRC}" "modules/js_ui.c" "modules/js_watchface.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "modules"
    REQUIRES board_hal core
    # cadran: js_watchface.c serializes into its face.bin format (face_format.h)
    # and its self-test calls cadran_face_load()/cadran_render() - this edge
    # goes only this direction, cadran has zero dependency back on unruh.
    PRIV_REQUIRES quickjs gfx cadran   # provide as managed component or components/quickjs
)
