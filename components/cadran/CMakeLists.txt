# Cadran - declarative watchface engine, see docs/design/cadran-watchface-
# engine.md. No dependency on unruh/quickjs: cadran_render() is meant to
# run without booting the JS engine at all.
idf_component_register(
    SRCS "loader.c" "render.c" "providers.c" "selftest.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES board_hal
)
