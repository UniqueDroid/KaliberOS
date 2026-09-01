# ESP-IDF component wrapper around the quickjs-ng submodule at
# third_party/quickjs (kept unmodified upstream, no ESP-IDF component
# structure of its own). Only the four core interpreter sources are built -
# no CLI (qjs.c/qjsc.c), no libc bindings, no generated CLI bytecode blobs.
set(QJS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/quickjs")

idf_component_register(
    SRCS
        "${QJS_DIR}/quickjs.c"
        "${QJS_DIR}/libregexp.c"
        "${QJS_DIR}/libunicode.c"
        "${QJS_DIR}/dtoa.c"
    INCLUDE_DIRS "${QJS_DIR}"
)

# quickjs.c relies on GCC/Clang extensions (computed gotos, statement
# expressions) that -std=gnu11 or -std=c11 with -Wpedantic would reject.
# The two -Wno-error flags below silence upstream quirks that ESP-IDF's
# default -Wall -Wextra -Werror project flags treat as hard errors:
# deliberate switch-case fallthrough without [[fallthrough]] annotations,
# a handful of int/int32_t pointer-type mismatches, and %x/%u/%d format
# specifiers written for a host where uint32_t is `unsigned int` - on
# Xtensa (this target) it's `long unsigned int`, so -Wformat flags every
# one of them. All harmless (same width everywhere this runs), not fixed
# in the vendored source, so a submodule update doesn't silently
# reintroduce a patched-away error.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-error=implicit-fallthrough
    -Wno-error=incompatible-pointer-types
    -Wno-error=maybe-uninitialized
    -Wno-error=format
    -Wno-error=format=
)
