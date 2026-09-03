#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "cadran_internal.h"
#include "gfx/text.h"

static const char *TAG = "cadran.render";

/* -------------------------------------------------------- framebuffer */

/* Draws the foreground color. E-ink: canonical fb is 1bpp MSB-first,
 * 1=white/0=black (matches jw_ui's clear(), which memsets 0xFF), so
 * "draw" clears the bit. AMOLED RGB565: draws black on the assumption of
 * a white-cleared background - provisional until the first RGB565 board
 * lands and defines a real palette (see design doc, no such board yet). */
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

static void draw_line(uint8_t *fb, const board_desc_t *b, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        set_px(fb, b, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_rect(uint8_t *fb, const board_desc_t *b, int x, int y, int w, int h, bool filled) {
    if (w <= 0 || h <= 0) return;
    if (filled) {
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++)
                set_px(fb, b, xx, yy);
    } else {
        draw_line(fb, b, x, y, x + w - 1, y);
        draw_line(fb, b, x, y + h - 1, x + w - 1, y + h - 1);
        draw_line(fb, b, x, y, x, y + h - 1);
        draw_line(fb, b, x + w - 1, y, x + w - 1, y + h - 1);
    }
}

/* angle_deg: 0 = 12 o'clock, clockwise (matches the time.*_angle providers). */
static void draw_hand(uint8_t *fb, const board_desc_t *b, const cadran_widget_rec_t *w,
                       int32_t angle_deg) {
    float rad = (float)angle_deg * (float)M_PI / 180.0f;
    int len = w->params[0];
    int x1 = w->x + (int)lroundf((float)len * sinf(rad));
    int y1 = w->y - (int)lroundf((float)len * cosf(rad));
    draw_line(fb, b, w->x, w->y, x1, y1);
}

/* TEXT widget: str_ref is a format string (design doc §4, "built-in
 * bitmap font + format string with {v}"); the first "{v}" is replaced
 * with the bound provider's value (string as-is, i32 via snprintf). No
 * bind_id (have_val false) leaves the format string as-is, literal - a
 * text widget doesn't have to be bound to anything. params[0], if
 * nonzero, is the gfx scale factor; the design doc's own widget example
 * doesn't set one, so 0/unset defaults to scale 1. */
static void draw_text(uint8_t *fb, const board_desc_t *b, const cadran_widget_rec_t *w,
                       const char *fmt, bool have_val, const cadran_value_t *val) {
    if (!fmt) return;
    char out[64];
    const char *ph = have_val ? strstr(fmt, "{v}") : NULL;
    if (ph) {
        char valstr[24];
        if (val->is_string) {
            strncpy(valstr, val->str, sizeof valstr - 1);
            valstr[sizeof valstr - 1] = '\0';
        } else {
            snprintf(valstr, sizeof valstr, "%ld", (long)val->i32);
        }
        snprintf(out, sizeof out, "%.*s%s%s", (int)(ph - fmt), fmt, valstr, ph + 3);
    } else {
        strncpy(out, fmt, sizeof out - 1);
        out[sizeof out - 1] = '\0';
    }
    int scale = w->params[0] > 0 ? w->params[0] : 1;
    gfx_draw_text(fb, b, w->x, w->y, out, scale);
}

/* ------------------------------------------------------------- render */

esp_err_t cadran_render(const cadran_face_t *face, uint8_t *fb, const board_desc_t *board) {
    if (!face || !fb || !board) return ESP_ERR_INVALID_ARG;

    const cadran_widget_rec_t *widgets = cadran_face_widgets(face);
    uint8_t n = cadran_face_widget_count(face);

    for (uint8_t i = 0; i < n; i++) {
        const cadran_widget_rec_t *w = &widgets[i];

        cadran_value_t val = {0};
        bool bound = w->bind_id != CADRAN_PROVIDER_NONE;
        bool have_val = bound && cadran_provider_get((cadran_provider_id_t)w->bind_id, board, &val);
        /* Design doc §3: an unavailable provider degrades to "skip the
         * widget", not an error for the whole face. */
        if (bound && !have_val) continue;

        switch ((cadran_widget_type_t)w->type) {
        case CADRAN_WIDGET_RECT:
            draw_rect(fb, board, w->x, w->y, w->params[0], w->params[1], w->params[2] != 0);
            break;
        case CADRAN_WIDGET_LINE:
            draw_line(fb, board, w->x, w->y, w->params[0], w->params[1]);
            break;
        case CADRAN_WIDGET_HAND:
            draw_hand(fb, board, w, have_val ? val.i32 : 0);
            break;
        case CADRAN_WIDGET_TEXT:
            draw_text(fb, board, w, cadran_face_string(face, w->str_ref), have_val, &val);
            break;
        case CADRAN_WIDGET_IMG:
        case CADRAN_WIDGET_IMG_DIGITS:
        case CADRAN_WIDGET_ARC:
        case CADRAN_WIDGET_IMG_LEVEL:
            /* Needs the atelier resource pipeline (image assets) - not
             * built yet (see main README roadmap). Skip rather than fail
             * the whole face. */
            ESP_LOGD(TAG, "widget %d: type %d not renderable yet, skipped", i, w->type);
            break;
        default:
            ESP_LOGW(TAG, "widget %d: unknown type %d", i, w->type);
            break;
        }
    }
    return ESP_OK;
}
