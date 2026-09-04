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
 * A drawing surface: fb is one stripe's buffer (docs/design/display-
 * regions.md), origin_y is that stripe's panel-absolute y, height is
 * its extent. For a non-striped board (caps.stripe_lines == 0), origin_y
 * is always 0 and height is board->caps.disp_h - the whole panel in one
 * "stripe", identical to what every caller did before this existed.
 */
typedef struct {
    uint8_t             *fb;
    const board_desc_t  *board;
    int                  origin_y;
    int                  height;
} gfx_ctx_t;

/**
 * Draws str into ctx->fb using the built-in 8x8 bitmap font (ASCII
 * 0x20-0x7e). x/y are panel-absolute (matches the Cadran widget-position
 * convention) - internally clipped to [ctx->origin_y, ctx->origin_y +
 * ctx->height), the same mechanism that already clips at the panel's own
 * edges, just with tighter bounds. A widget/string straddling a stripe
 * boundary draws whatever fraction falls in the current stripe, nothing
 * more. Each font pixel is drawn as a scale x scale block; scale < 1 is
 * clamped to 1. Bytes outside 0x20-0x7e render as a blank cell instead of
 * being skipped, so string layout stays predictable.
 *
 * Every destination pixel is bounds-checked individually, not just the
 * glyph's origin - at scale > 1 a glyph can straddle an edge (panel or
 * stripe) that a per-glyph check would miss. Out-of-range coordinates/
 * strings are silently clipped.
 */
void gfx_draw_text(const gfx_ctx_t *ctx, int x, int y, const char *str, int scale);

#ifdef __cplusplus
}
#endif
