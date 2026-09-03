/**
 * Kaliber gfx — text rendering.
 *
 * No dependency on unruh/quickjs (mirrors components/cadran's independence
 * from the JS engine), so Cadran can adopt this rasterizer later without
 * pulling in an engine dependency. Only depends on board_hal for the
 * canonical framebuffer layout.
 */
#pragma once

#include <stdint.h>
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draws str into fb using the built-in 8x8 bitmap font (ASCII 0x20-0x7e).
 * Origin top-left, y grows downward (matches the Cadran widget-position
 * convention). Each font pixel is drawn as a scale x scale block; scale < 1
 * is clamped to 1. Bytes outside 0x20-0x7e render as a blank cell instead
 * of being skipped, so string layout stays predictable.
 *
 * Every destination pixel is bounds-checked individually, not just the
 * glyph's origin - at scale > 1 a glyph can straddle the edge of the
 * framebuffer, and that's exactly the case a per-glyph check would miss.
 * Out-of-range coordinates/strings are silently clipped.
 */
void gfx_draw_text(uint8_t *fb, const board_desc_t *board,
                    int x, int y, const char *str, int scale);

#ifdef __cplusplus
}
#endif
