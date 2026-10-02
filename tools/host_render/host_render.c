/**
 * Host-side renderer for gfx + Cadran's render path (project chat
 * 2026-10-02, Simon via Jan: "stop reasoning about layouts by hand, build
 * a host renderer"). Links the REAL production sources -
 * components/gfx/text.c, components/gfx/native_screens.c,
 * components/cadran/{loader,render,providers}.c - against tiny host
 * stand-ins for board_hal's board_desc_t and ESP-IDF's esp_err.h/
 * esp_log.h (see stub/). No FreeRTOS, no ESP-IDF, no device. Pure C,
 * build with build.sh.
 *
 * Covers two things, both host-testable because they only ever touch
 * gfx/cadran + a gfx_ctx_t:
 *   - the default watchface's TEXT widget (large-font-role resolution,
 *     the reported "only one digit shows up" symptom)
 *   - the native screens (NO APPS/MENU/WAKE CHECK/APP label/SYNC),
 *     factored into components/gfx/native_screens.c specifically so this
 *     tool and the real launcher/net_svc call the identical code - the
 *     reported "left edge clipped"/"stuck top-left" symptoms.
 *
 * Deliberately OUT of scope (needs real hardware, not a software bug by
 * definition): the SSD1681/SH8601 display driver's own blit_region()
 * address-window logic, and anything about the physical stripe transfer.
 * If a symptom does NOT reproduce here, that's the signal to go look
 * there next, not a reason to assume this tool is wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board_hal/board.h"
#include "gfx/text.h"
#include "gfx/native_screens.h"
#include "cadran/cadran.h"
#include "cadran/face_format.h"
#include "png_write.h"

/* ------------------------------------------------------- host boards */

static uint32_t fake_battery_mv(void) { return 3800; } /* -> ~56% */

static const power_ops_t s_power = { .battery_mv = fake_battery_mv };

static const board_desc_t s_board_watchy = {
    .name = "watchy_v3 (host)",
    .power = &s_power,
    .caps = { .disp_w = 200, .disp_h = 200, .disp_kind = DISP_EINK_1BIT, .stripe_lines = 0 },
};

static const board_desc_t s_board_c6 = {
    .name = "waveshare_c6_amoled (host)",
    .power = &s_power,
    .caps = { .disp_w = 410, .disp_h = 502, .disp_kind = DISP_AMOLED_RGB565, .stripe_lines = 0 },
};

/* Oversized virtual "panel", wide enough that 11 glyphs at 64px each
 * (704px) never clips against disp_w - exists only for
 * render_font_glyph_sheet(), not a real board. */
static const board_desc_t s_board_sheet = {
    .name = "glyph-sheet (host, not a real board)",
    .power = &s_power,
    .caps = { .disp_w = 800, .disp_h = 120, .disp_kind = DISP_AMOLED_RGB565, .stripe_lines = 0 },
};

static size_t fb_bytes(const board_desc_t *b) {
    if (b->caps.disp_kind == DISP_EINK_1BIT) {
        size_t stride = ((size_t)b->caps.disp_w + 7) / 8;
        return stride * b->caps.disp_h;
    }
    return (size_t)b->caps.disp_w * b->caps.disp_h * 2;
}

/* ------------------------------------------------------------- output */

static void clear_white(uint8_t *fb, const board_desc_t *b) {
    memset(fb, 0xFF, fb_bytes(b)); /* matches jw_ui's clear(), see gfx/text.c */
}

static int save_png(const char *name, const uint8_t *fb, const board_desc_t *b) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", getenv("HOST_RENDER_OUTDIR") ? getenv("HOST_RENDER_OUTDIR") : ".", name);

    int w = b->caps.disp_w, h = b->caps.disp_h;
    int rc;
    if (b->caps.disp_kind == DISP_EINK_1BIT) {
        uint8_t *gray = malloc((size_t)w * h);
        size_t stride = ((size_t)w + 7) / 8;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t byte = fb[(size_t)y * stride + x / 8];
                int bit = (byte >> (7 - (x % 8))) & 1; /* 1 = white (unset) */
                gray[(size_t)y * w + x] = bit ? 255 : 0;
            }
        }
        rc = png_write_gray8(path, gray, w, h);
        free(gray);
    } else {
        uint8_t *rgb = malloc((size_t)w * h * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t idx = ((size_t)y * w + x) * 2;
                uint16_t px = (uint16_t)(fb[idx] << 8 | fb[idx + 1]);
                uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
                uint8_t bl = (uint8_t)((px & 0x1F) * 255 / 31);
                size_t o = ((size_t)y * w + x) * 3;
                rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = bl;
            }
        }
        rc = png_write_rgb8(path, rgb, w, h);
        free(rgb);
    }
    if (rc == 0) printf("wrote %s (%dx%d)\n", path, w, h);
    else fprintf(stderr, "FAILED to write %s\n", path);
    return rc;
}

/* ------------------------------------------------- default watchface */

/* Builds a face.bin buffer in memory, same shape cadran/selftest.c's
 * hand-authored test face uses, so cadran_face_load() is exercised
 * unmodified. Widget x/y/params replicate examples/watchfaces/default/
 * app.js's build() formula exactly (charW = floor(ctx.w/6), w = 5*charW,
 * h = floor(charW*1.5), centered) - that JS is deterministic and already
 * read, not re-guessed; this is the same arithmetic a real build() would
 * produce for this board, not a stand-in for it. */
static esp_err_t build_default_face(const board_desc_t *b, uint8_t **out_buf, size_t *out_len) {
    int char_w = b->caps.disp_w / 6;
    int w = 5 * char_w;
    int h = (int)(char_w * 1.5);
    int x = (b->caps.disp_w - w) / 2;
    int y = (b->caps.disp_h - h) / 2;

    static const char fmt[] = "{v}";
    cadran_widget_rec_t widget = {
        .type = CADRAN_WIDGET_TEXT, .bind_id = CADRAN_PROVIDER_TIME_HM,
        .x = (int16_t)x, .y = (int16_t)y,
        .params = { CADRAN_FONT_LARGE, 0, 0, 0 },
        .str_ref = 0,
    };

    cadran_header_t hdr = { .magic = {'C','D','R','N'}, .abi = CADRAN_ABI, .widget_count = 1, .flags = 0 };
    size_t total = sizeof hdr + sizeof fmt + sizeof widget;
    uint8_t *buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;
    uint8_t *p = buf;
    memcpy(p, &hdr, sizeof hdr); p += sizeof hdr;
    memcpy(p, fmt, sizeof fmt); p += sizeof fmt;
    memcpy(p, &widget, sizeof widget);
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

static void render_default_face(const board_desc_t *b, const char *tag) {
    uint8_t *face_buf; size_t face_len;
    if (build_default_face(b, &face_buf, &face_len) != ESP_OK) { fprintf(stderr, "build_default_face failed\n"); return; }

    cadran_face_t *face = NULL;
    esp_err_t err = cadran_face_load(face_buf, face_len, &face);
    free(face_buf);
    if (err != ESP_OK) { fprintf(stderr, "cadran_face_load: %s\n", esp_err_to_name(err)); return; }

    uint8_t *fb = malloc(fb_bytes(b));
    clear_white(fb, b);
    gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = 0, .height = b->caps.disp_h };
    err = cadran_render(face, &ctx);
    if (err != ESP_OK) fprintf(stderr, "cadran_render: %s\n", esp_err_to_name(err));

    char name[128];
    snprintf(name, sizeof name, "%s_default_face.png", tag);
    save_png(name, fb, b);

    free(fb);
    cadran_face_free(face);
}

/* ------------------------------------------------------- native screens */

static void render_native_screens(const board_desc_t *b, const char *tag) {
    uint8_t *fb = malloc(fb_bytes(b));
    gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = 0, .height = b->caps.disp_h };
    char name[128];

    clear_white(fb, b); gfx_screens_draw_no_apps(&ctx);
    snprintf(name, sizeof name, "%s_no_apps.png", tag); save_png(name, fb, b);

    clear_white(fb, b); gfx_screens_draw_menu(&ctx);
    snprintf(name, sizeof name, "%s_menu.png", tag); save_png(name, fb, b);

    clear_white(fb, b); gfx_screens_draw_wake_check(&ctx, "tick-wake: MENU -> WATCHFACE: PASS");
    snprintf(name, sizeof name, "%s_wake_check.png", tag); save_png(name, fb, b);

    /* App label overlay (the hardcoded-(10,190) bug) - drawn over
     * whatever an app would have rendered; shown here over a blank
     * white frame so the label itself is unambiguous. */
    clear_white(fb, b); gfx_screens_draw_app_label(&ctx, "de.jan.hello");
    snprintf(name, sizeof name, "%s_app_label.png", tag); save_png(name, fb, b);

    clear_white(fb, b);
    gfx_screens_draw_sync(&ctx, "KaliberOS-CCA5", "QX7K2M9P",
                           "192.168.4.1", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd");
    snprintf(name, sizeof name, "%s_sync.png", tag); save_png(name, fb, b);

    free(fb);
}

/* Direct gfx_draw_text_font() test, bypassing Cadran entirely - renders
 * every glyph gfx_font_time_large actually has (0-9 plus ':'), one row,
 * at native resolution. Checks the font DATA itself (tools/fontgen's
 * generated table), not the role-resolution/widget-positioning logic the
 * other tests already exercise - a generation-time glyph defect (this
 * project has hit that before: gfx_font8x8.h's '1'/'4'/'7') wouldn't show
 * up in a render that only happens to need digits that came out clean. */
static void render_font_glyph_sheet(const board_desc_t *b, const char *tag) {
    uint8_t *fb = malloc(fb_bytes(b));
    clear_white(fb, b);
    gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = 0, .height = b->caps.disp_h };
    gfx_draw_text_font(&ctx, 2, 2, "0123456789:", 1, &gfx_font_time_large);
    char name[128];
    snprintf(name, sizeof name, "%s_font_time_large_glyphsheet.png", tag);
    save_png(name, fb, b);
    free(fb);
}

/* Reference image for main.c's display_path_selftest() - identical shapes
 * (border at the panel edges, one diagonal, a grid with horizontal lines
 * at every stripe boundary), rendered here instead of on real hardware so
 * a device photo of the same pattern can be compared against a known-
 * correct baseline (project chat 2026-10-02 - isolates gfx's own line
 * drawing, already exercised elsewhere in this file, from the display
 * driver's transfer path, which only the real device can test). */
static void render_display_path_test(const board_desc_t *b, const char *tag) {
    uint8_t *fb = malloc(fb_bytes(b));
    clear_white(fb, b);
    gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = 0, .height = b->caps.disp_h };
    int w = b->caps.disp_w, h = b->caps.disp_h;
    /* The real waveshare_c6_amoled board.c sets stripe_lines=32 - hardcoded
     * here rather than read from b->caps.stripe_lines, since the host
     * boards above use 0 (whole-panel, no stripe loop needed for every
     * other test in this file) and this specific grid only means anything
     * against the real device's actual stripe boundaries. */
    uint16_t stripe = 32;

    gfx_draw_hline(&ctx, 0, w - 1, 0);
    gfx_draw_hline(&ctx, 0, w - 1, h - 1);
    gfx_draw_line(&ctx, 0, 0, 0, h - 1);
    gfx_draw_line(&ctx, w - 1, 0, w - 1, h - 1);
    gfx_draw_line(&ctx, 0, 0, w - 1, h - 1);
    for (int gy = 0; gy < h; gy += stripe) gfx_draw_hline(&ctx, 0, w - 1, gy);
    for (int gx = 0; gx < w; gx += 50) gfx_draw_line(&ctx, gx, 0, gx, h - 1);

    char name[128];
    snprintf(name, sizeof name, "%s_display_path_test.png", tag);
    save_png(name, fb, b);
    free(fb);
}

int main(void) {
    time_t now = time(NULL);
    printf("host_render: build_time~=%ld (TIME_HM provider uses the real system clock)\n", (long)now);

    render_default_face(&s_board_c6, "c6_410x502");
    render_default_face(&s_board_watchy, "watchy_200x200");
    render_native_screens(&s_board_c6, "c6_410x502");
    render_native_screens(&s_board_watchy, "watchy_200x200");
    render_font_glyph_sheet(&s_board_sheet, "glyphsheet");
    render_display_path_test(&s_board_c6, "c6_410x502");

    return 0;
}
