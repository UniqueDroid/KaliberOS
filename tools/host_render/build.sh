#!/bin/sh
# Builds + runs the host renderer. No ESP-IDF, plain gcc + zlib.
# Usage: tools/host_render/build.sh [output-dir]
set -e
cd "$(dirname "$0")/../.."
ROOT="$PWD"
OUT="${1:-$ROOT/tools/host_render/out}"
mkdir -p "$OUT"

gcc -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wextra -O1 -g \
    -DCONFIG_KALIBER_NET_AP_PASSWORD='""' \
    -I "$ROOT/tools/host_render/stub" \
    -I "$ROOT/components/board_hal/include" \
    -I "$ROOT/components/gfx/include" \
    -I "$ROOT/components/gfx" \
    -I "$ROOT/components/cadran/include" \
    -I "$ROOT/components/cadran" \
    "$ROOT/tools/host_render/host_render.c" \
    "$ROOT/tools/host_render/png_write.c" \
    "$ROOT/components/gfx/text.c" \
    "$ROOT/components/gfx/native_screens.c" \
    "$ROOT/components/cadran/loader.c" \
    "$ROOT/components/cadran/render.c" \
    "$ROOT/components/cadran/providers.c" \
    -lm -lz \
    -o "$OUT/host_render"

HOST_RENDER_OUTDIR="$OUT" "$OUT/host_render"
