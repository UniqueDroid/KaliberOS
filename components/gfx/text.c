#include "gfx/text.h"
#include "gfx_font8x8.h"

#define FONT_FIRST 0x20
#define FONT_LAST  0x7e
#define FONT_W 8
#define FONT_H 8

/* Same fb-write logic as cadran/render.c's private set_px (E-ink: clearing
 * a bit draws; AMOLED RGB565: writes black on a white-cleared background,
 * provisional until a real RGB565 board defines a palette) - kept as its
 * own static copy rather than shared, matching how cadran keeps its own.
 *
 * x/y are panel-absolute; clipped against the panel edge AND the current
 * stripe (docs/design/display-regions.md) - for a non-striped board
 * (ctx->origin_y == 0, ctx->height == board->caps.disp_h) the second
 * check is a no-op, identical to before this existed. */
static inline void set_px(const gfx_ctx_t *ctx, int x, int y) {
    const board_desc_t *b = ctx->board;
    if (x < 0 || x >= b->caps.disp_w) return;
    int local_y = y - ctx->origin_y;
    if (local_y < 0 || local_y >= ctx->height) return;
    if (b->caps.disp_kind == DISP_EINK_1BIT) {
        size_t stride = ((size_t)b->caps.disp_w + 7) / 8;
        size_t byte_i = (size_t)local_y * stride + (size_t)x / 8;
        uint8_t mask = 0x80 >> (x % 8);
        ctx->fb[byte_i] &= (uint8_t)~mask;
    } else {
        size_t idx = ((size_t)local_y * b->caps.disp_w + (size_t)x) * 2;
        ctx->fb[idx] = 0x00;
        ctx->fb[idx + 1] = 0x00;
    }
}

void gfx_draw_text(const gfx_ctx_t *ctx, int x, int y, const char *str, int scale) {
    if (!ctx || !ctx->fb || !ctx->board || !str) return;
    if (scale < 1) scale = 1;

    int pen_x = x;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        unsigned char c = *p;
        const uint8_t *glyph = (c >= FONT_FIRST && c <= FONT_LAST)
                                    ? font8x8[c - FONT_FIRST] : NULL;
        if (glyph) {
            for (int gy = 0; gy < FONT_H; gy++) {
                uint8_t row = glyph[gy];
                for (int gx = 0; gx < FONT_W; gx++) {
                    if (!(row & (0x80 >> gx))) continue;
                    int px0 = pen_x + gx * scale;
                    int py0 = y + gy * scale;
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            set_px(ctx, px0 + sx, py0 + sy);
                }
            }
        }
        pen_x += FONT_W * scale;
    }
}
