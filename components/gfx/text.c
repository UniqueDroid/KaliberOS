#include "gfx/text.h"
#include "gfx_font8x8.h"

#define FONT_FIRST 0x20
#define FONT_LAST  0x7e
#define FONT_W 8
#define FONT_H 8

/* Same fb-write logic as cadran/render.c's private set_px (E-ink: clearing
 * a bit draws; AMOLED RGB565: writes black on a white-cleared background,
 * provisional until a real RGB565 board defines a palette) - kept as its
 * own static copy rather than shared, matching how cadran keeps its own. */
static inline void set_px(uint8_t *fb, const board_desc_t *b, int x, int y) {
    if (x < 0 || y < 0 || x >= b->caps.disp_w || y >= b->caps.disp_h) return;
    if (b->caps.disp_kind == DISP_EINK_1BIT) {
        size_t stride = ((size_t)b->caps.disp_w + 7) / 8;
        size_t byte_i = (size_t)y * stride + (size_t)x / 8;
        uint8_t mask = 0x80 >> (x % 8);
        fb[byte_i] &= (uint8_t)~mask;
    } else {
        size_t idx = ((size_t)y * b->caps.disp_w + (size_t)x) * 2;
        fb[idx] = 0x00;
        fb[idx + 1] = 0x00;
    }
}

void gfx_draw_text(uint8_t *fb, const board_desc_t *board,
                    int x, int y, const char *str, int scale) {
    if (!fb || !board || !str) return;
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
                            set_px(fb, board, px0 + sx, py0 + sy);
                }
            }
        }
        pen_x += FONT_W * scale;
    }
}
