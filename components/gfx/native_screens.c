#include <string.h>
#include <stdio.h>
#include "gfx/native_screens.h"

int gfx_screen_margin(const board_desc_t *b) {
    /* ~5% (was ~2.5% - project chat 2026-10-02, Jan/Simon: the old ratio
     * read as "debug terminal" on the C6's tall 410x502 panel, content
     * pinned in a top corner with most of the panel left blank). */
    int m = b->caps.disp_w / 20;
    return m < 4 ? 4 : m;
}

int gfx_screen_headline_y(const board_desc_t *b) {
    int y = b->caps.disp_h / 12;
    return y < 10 ? 10 : y;
}

int gfx_screen_line_gap(const board_desc_t *b) {
    int g = b->caps.disp_h / 30;
    return g < 15 ? 15 : g;
}

/* Vertical pitch for a body line of the given pixel height (8 for the
 * built-in font, or a gfx_font_t's own .height) - 1.5x reads as
 * comfortably spaced without the screen's content needing disp_h at all
 * (project chat 2026-10-02), unlike gfx_screen_line_gap(), which only
 * knows the panel. Takes a height, not a gfx_font_t*, so it still works
 * when fits_font_16() fell back to NULL (8x8 font). */
static int body_line_gap(int line_h) {
    return line_h + line_h / 2;
}

const gfx_font_t *gfx_headline_font(const board_desc_t *b, const char *str, int margin) {
    int avail = b->caps.disp_w - 2 * margin;
    int w32 = (int)strlen(str) * gfx_font_32.width;
    return (w32 <= avail) ? &gfx_font_32 : &gfx_font_16;
}

/* NULL means "this board's disp_w can't fit widest_str in gfx_font_16 at
 * this margin - fall back to the built-in 8x8 font via gfx_draw_text()".
 * Same fits-or-fallback shape as gfx_headline_font(), just against
 * gfx_font_16 instead of choosing between two gfx_font_t sizes - body
 * text has nowhere smaller to fall back to except the 8x8 font, which
 * isn't a gfx_font_t (see gfx_draw_detail_line()). */
static const gfx_font_t *fits_font_16(const board_desc_t *b, const char *widest_str, int margin) {
    int avail = b->caps.disp_w - 2 * margin;
    return ((int)strlen(widest_str) * gfx_font_16.width <= avail) ? &gfx_font_16 : NULL;
}

const gfx_font_t *gfx_detail_font(const board_desc_t *b, int margin) {
    /* "KEY:  " + 16 hex chars - the widest line gfx_screens_draw_sync()
     * ever draws. */
    return fits_font_16(b, "KEY:  0123456789abcdef", margin);
}

void gfx_draw_detail_line(const gfx_ctx_t *ctx, int x, int y, const char *s,
                           const gfx_font_t *font) {
    if (font) gfx_draw_text_font(ctx, x, y, s, 1, font);
    else gfx_draw_text(ctx, x, y, s, 1);
}

/* Headline + rule + N body lines, vertically centered as one block
 * (project chat 2026-10-02: Jan picked this over a plain vcenter and
 * over vcenter-with-bigger-body-but-no-rule - "optisch C am besten...
 * sollte durch echte Fonts später ersetzt werden", i.e. the accepted
 * stopgap until docs/design/base-system.md's Phase 4 design system
 * exists, not a final look). Returns the y just below the rule, so the
 * caller draws its own body lines relative to that at whatever spacing
 * fits its content (count/font differs per screen below). */
static int draw_headline_and_rule(const gfx_ctx_t *ctx, const char *headline, int block_h) {
    const board_desc_t *b = ctx->board;
    int margin = gfx_screen_margin(b);
    const gfx_font_t *hf = gfx_headline_font(b, headline, margin);
    int y0 = (b->caps.disp_h - block_h) / 2;
    if (y0 < margin) y0 = margin; /* a very tall block (e.g. sync's many
                                     * detail lines) still starts on-panel */
    gfx_draw_text_font(ctx, margin, y0, headline, 1, hf);
    int rule_y = y0 + hf->height + hf->height / 4;
    gfx_draw_hline(ctx, margin, b->caps.disp_w - margin, rule_y);
    return rule_y;
}

void gfx_screens_draw_no_apps(const gfx_ctx_t *ctx) {
    const board_desc_t *b = ctx->board;
    const gfx_font_t *hf = gfx_headline_font(b, "NO APPS", gfx_screen_margin(b));
    int pad = hf->height / 4, body_h = 16 /* "atelier push" at scale 2 */;
    int rule_y = draw_headline_and_rule(ctx, "NO APPS", hf->height + pad + pad + body_h);
    gfx_draw_text(ctx, gfx_screen_margin(b), rule_y + pad * 2, "atelier push", 2);
}

void gfx_screens_draw_menu(const gfx_ctx_t *ctx) {
    const board_desc_t *b = ctx->board;
    int margin = gfx_screen_margin(b);
    const gfx_font_t *hf = gfx_headline_font(b, "MENU", margin);
    /* "BACK:   watchface" is the widest of the three body lines - same
     * fits-or-fallback-to-8x8 shape gfx_detail_font() uses for SYNC's
     * detail lines, so this keeps fitting on watchy_v3's 200px panel
     * instead of running off the right edge the way a hardcoded
     * gfx_font_16 did (found immediately via the host renderer, 2026-10-02
     * - exactly the kind of regression it exists to catch). */
    const gfx_font_t *bf = fits_font_16(b, "BACK:   watchface", margin);
    int gap = body_line_gap(bf ? bf->height : 8), pad = hf->height / 4;
    int rule_y = draw_headline_and_rule(ctx, "MENU", hf->height + pad + pad + gap * 3);
    gfx_draw_detail_line(ctx, margin, rule_y + gap, "SELECT: open", bf);
    gfx_draw_detail_line(ctx, margin, rule_y + gap * 2, "BACK:   watchface", bf);
    gfx_draw_detail_line(ctx, margin, rule_y + gap * 3, "DOWN:   install", bf);
}

void gfx_screens_draw_wake_check(const gfx_ctx_t *ctx, const char *msg) {
    const board_desc_t *b = ctx->board;
    const gfx_font_t *hf = gfx_headline_font(b, "WAKE CHECK", gfx_screen_margin(b));
    int pad = hf->height / 4, body_h = 8 /* msg at the 8x8 font, scale 1 */;
    int rule_y = draw_headline_and_rule(ctx, "WAKE CHECK", hf->height + pad + pad + body_h);
    gfx_draw_text(ctx, gfx_screen_margin(b), rule_y + pad * 2, msg, 1);
}

void gfx_screens_draw_app_label(const gfx_ctx_t *ctx, const char *app_id) {
    const board_desc_t *b = ctx->board;
    char label[80];
    snprintf(label, sizeof label, "APP: %s", app_id);
    int margin = gfx_screen_margin(b);
    /* 8x8 font at scale 1 - bottom-left, relative to this board's own
     * panel height (was hardcoded (10,190) for watchy_v3's 200px panel -
     * sat off the bottom of a shorter non-existent panel and nowhere near
     * the bottom on the C6's 502px one). */
    gfx_draw_text(ctx, margin, b->caps.disp_h - margin - 8, label, 1);
}

void gfx_screens_draw_sync(const gfx_ctx_t *ctx, const char *ssid,
                            const char *pass, const char *ip, const char *key) {
    const board_desc_t *b = ctx->board;
    char line[80];
    int margin = gfx_screen_margin(b), gap = gfx_screen_line_gap(b);
    const gfx_font_t *dfont = gfx_detail_font(b, margin);
    if (dfont && gap < dfont->height + 4) gap = dfont->height + 4;
    /* Vertically centered as one block (project chat 2026-10-02 - same
     * "pinned in a top corner" complaint the other native screens had,
     * same fix). "SYNC" itself is drawn with the built-in 8x8 font at
     * scale 3 (24px tall), not a gfx_font_t - block height approximates
     * that line by its own pixel height, not a headline-font value. */
    int block_h = 24 + gap * 10; /* SYNC headline + 3 detail lines + 4 key lines, same spacing as before */
    int y0 = (b->caps.disp_h - block_h) / 2;
    if (y0 < margin) y0 = margin;

    gfx_draw_text(ctx, margin, y0, "SYNC", 3);
    snprintf(line, sizeof line, "SSID: %s", ssid);
    gfx_draw_detail_line(ctx, margin, y0 + gap * 3, line, dfont);
    snprintf(line, sizeof line, "PASS: %s", pass);
    gfx_draw_detail_line(ctx, margin, y0 + gap * 4, line, dfont);
    snprintf(line, sizeof line, "IP:   %s:8080", ip);
    gfx_draw_detail_line(ctx, margin, y0 + gap * 5, line, dfont);

    char keybuf[65];
    strncpy(keybuf, key, sizeof keybuf - 1);
    keybuf[sizeof keybuf - 1] = '\0';
    for (int ki = 0; ki < 4; ki++) {
        snprintf(line, sizeof line, "%s%.16s", ki == 0 ? "KEY:  " : "      ", keybuf + ki * 16);
        gfx_draw_detail_line(ctx, margin, y0 + gap * (7 + ki), line, dfont);
    }
}
