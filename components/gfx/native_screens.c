#include <string.h>
#include <stdio.h>
#include "gfx/native_screens.h"

int gfx_screen_margin(const board_desc_t *b) {
    int m = b->caps.disp_w / 40;
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

const gfx_font_t *gfx_headline_font(const board_desc_t *b, const char *str, int margin) {
    int avail = b->caps.disp_w - 2 * margin;
    int w32 = (int)strlen(str) * gfx_font_32.width;
    return (w32 <= avail) ? &gfx_font_32 : &gfx_font_16;
}

const gfx_font_t *gfx_detail_font(const board_desc_t *b, int margin) {
    int avail = b->caps.disp_w - 2 * margin;
    if (22 * gfx_font_16.width <= avail) return &gfx_font_16;
    return NULL;
}

void gfx_draw_detail_line(const gfx_ctx_t *ctx, int x, int y, const char *s,
                           const gfx_font_t *font) {
    if (font) gfx_draw_text_font(ctx, x, y, s, 1, font);
    else gfx_draw_text(ctx, x, y, s, 1);
}

void gfx_screens_draw_no_apps(const gfx_ctx_t *ctx) {
    const board_desc_t *b = ctx->board;
    int margin = gfx_screen_margin(b), y0 = gfx_screen_headline_y(b), gap = gfx_screen_line_gap(b);
    gfx_draw_text_font(ctx, margin, y0, "NO APPS", 1, gfx_headline_font(b, "NO APPS", margin));
    gfx_draw_text(ctx, margin, y0 + gap * 3, "atelier push", 2);
}

void gfx_screens_draw_menu(const gfx_ctx_t *ctx) {
    const board_desc_t *b = ctx->board;
    int margin = gfx_screen_margin(b), y0 = gfx_screen_headline_y(b), gap = gfx_screen_line_gap(b);
    gfx_draw_text_font(ctx, margin, y0, "MENU", 1, gfx_headline_font(b, "MENU", margin));
    gfx_draw_text(ctx, margin, y0 + gap * 3, "SELECT: open", 1);
    gfx_draw_text(ctx, margin, y0 + gap * 4, "BACK:   watchface", 1);
    gfx_draw_text(ctx, margin, y0 + gap * 5, "DOWN:   install", 1);
}

void gfx_screens_draw_wake_check(const gfx_ctx_t *ctx, const char *msg) {
    const board_desc_t *b = ctx->board;
    int margin = gfx_screen_margin(b), y0 = gfx_screen_headline_y(b), gap = gfx_screen_line_gap(b);
    gfx_draw_text_font(ctx, margin, y0, "WAKE CHECK", 1, gfx_headline_font(b, "WAKE CHECK", margin));
    gfx_draw_text(ctx, margin, y0 + gap * 2, msg, 1);
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
    int y0 = margin;

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
