# gfx - text rasterization. No dependency on unruh/quickjs, so cadran could
# adopt this later without pulling in an engine dependency (see components/
# cadran/CMakeLists.txt, same rationale).
idf_component_register(
    SRCS "text.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES board_hal
)
