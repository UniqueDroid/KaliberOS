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

/**
 * A second bitmap font, rasterized at its own native resolution instead
 * of integer-upscaled from the 8x8 one (project chat 2026-09-30:
 * upscaling "ergibt Klötze" - blocky, unreadable at any scale beyond
 * 2-3x; a bigger font rasterized from the same TTF at its own size
 * looks like a font, not upscaled pixels). No runtime rasterizer, no
 * antialiasing - these are still fixed 1-bit glyph tables, generated
 * offline by tools/fontgen/gen_bitmap_font.py, just at a larger native
 * size than gfx_font8x8.h's. `scale` still works the same way
 * (`scale=1` draws at native resolution, `scale=2` doubles every pixel,
 * etc.) - upscaling a *bigger* base font by a *smaller* multiplier
 * looks fine, the blockiness only shows up when the multiplier itself
 * is large relative to the base glyph.
 */
typedef struct {
    const uint8_t *data;    /* flat, glyph-major: height rows of row_bytes
                              * each per glyph, MSB-first - see whichever
                              * gfx_fontNxN.h generated this */
    uint16_t       width, height;
    uint8_t        row_bytes;
    uint8_t        first, last; /* ASCII range covered, e.g. 0x20/0x7e */
} gfx_font_t;

extern const gfx_font_t gfx_font_16; /* components/gfx/gfx_font16x16.h */
extern const gfx_font_t gfx_font_32; /* components/gfx/gfx_font32x32.h */

/* Digits + colon only (0x30-0x3a, contiguous in ASCII - '0'-'9' then ':'
 * right after), not full ASCII - a clock face never needs letters at
 * this size, and restricting the range is what makes a size this big
 * affordable in flash (11 glyphs vs. 95 - see gen_bitmap_font.py's own
 * comment). Any character outside 0x30-0x3a renders as a blank cell,
 * same as gfx_draw_text()'s out-of-range behavior - not usable for
 * general text, only for the digit/colon strings a time display needs.
 * Named for its intended role (docs/design/cadran-watchface-engine.md
 * §5a's "large"), not its pixel size, on purpose - that's the thing a
 * face is meant to ask for. */
extern const gfx_font_t gfx_font_time_large; /* components/gfx/gfx_font96x96.h */

/* Same contract as gfx_draw_text(), plus an explicit font - use this for
 * anything that needs to be legible at a glance (headlines, the
 * watchface's own time display) instead of upscaling the 8x8 font
 * further than it can bear. */
void gfx_draw_text_font(const gfx_ctx_t *ctx, int x, int y, const char *str,
                         int scale, const gfx_font_t *font);

#ifdef __cplusplus
}
#endif
