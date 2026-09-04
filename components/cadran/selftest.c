#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cadran_internal.h"

static const char *TAG = "cadran.selftest";

/* Blit the test face to the real panel, not just check pixels in RAM.
 * Off by default - every extra e-ink refresh has a visible flicker/wear
 * cost, not something normal boots should pay. Flip to 1 for a hardware
 * verification pass, matching this project's track record: SPI init, the
 * budget-timeout classification and the '1' glyph all looked correct in
 * review and were wrong on real hardware - a log line saying "pixels
 * drawn" isn't as trustworthy as actually seeing them. */
#define CADRAN_SELFTEST_BLIT_TO_PANEL 0

/* Hand-authored face per design doc §9 step 1: a small in-memory face.bin
 * exercising every buildable widget type (rect, line, hand) plus a text
 * widget with a str_ref, to prove the string table round-trips even
 * though text isn't rendered yet. */
void cadran_selftest(void) {
    static const char str_batt[] = "batt {v}%";
    size_t str_len = sizeof str_batt; /* includes NUL */

    cadran_widget_rec_t widgets[4] = {0};
    widgets[0] = (cadran_widget_rec_t){
        .type = CADRAN_WIDGET_RECT, .bind_id = CADRAN_PROVIDER_NONE,
        .x = 2, .y = 2, .params = {20, 10, 1, 0}, .str_ref = CADRAN_STR_NONE,
    };
    widgets[1] = (cadran_widget_rec_t){
        .type = CADRAN_WIDGET_LINE, .bind_id = CADRAN_PROVIDER_NONE,
        .x = 0, .y = 0, .params = {10, 10, 0, 0}, .str_ref = CADRAN_STR_NONE,
    };
    widgets[2] = (cadran_widget_rec_t){
        .type = CADRAN_WIDGET_HAND, .bind_id = CADRAN_PROVIDER_TIME_MIN_ANGLE,
        .x = 50, .y = 50, .params = {30, 0, 0, 0}, .str_ref = CADRAN_STR_NONE,
    };
    widgets[3] = (cadran_widget_rec_t){
        .type = CADRAN_WIDGET_TEXT, .bind_id = CADRAN_PROVIDER_BATTERY_PCT,
        .x = 5, .y = 5, .params = {0, 0, 0, 0}, .str_ref = 0,
    };

    cadran_header_t hdr = {
        .magic = {'C', 'D', 'R', 'N'}, .abi = CADRAN_ABI,
        .widget_count = 4, .flags = 0,
    };

    size_t total = sizeof hdr + str_len + sizeof widgets;
    uint8_t *buf = malloc(total);
    if (!buf) { ESP_LOGE(TAG, "FAIL: no mem for test blob"); return; }
    uint8_t *p = buf;
    memcpy(p, &hdr, sizeof hdr);         p += sizeof hdr;
    memcpy(p, str_batt, str_len);        p += str_len;
    memcpy(p, widgets, sizeof widgets);

    cadran_face_t *face = NULL;
    esp_err_t err = cadran_face_load(buf, total, &face);
    free(buf); /* loader copies internally */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: load: %s", esp_err_to_name(err));
        return;
    }
    if (cadran_face_widget_count(face) != 4) {
        ESP_LOGE(TAG, "FAIL: widget_count %d != 4", cadran_face_widget_count(face));
        cadran_face_free(face);
        return;
    }
    const char *s = cadran_face_string(face, 0);
    if (!s || strcmp(s, str_batt) != 0) {
        ESP_LOGE(TAG, "FAIL: string table round-trip: got %s", s ? s : "(null)");
        cadran_face_free(face);
        return;
    }

    const board_desc_t *b = board_get();
    uint8_t *fb = calloc(1, board_fb_size());
    if (!fb) {
        ESP_LOGE(TAG, "FAIL: no mem for scratch fb");
        cadran_face_free(face);
        return;
    }
    memset(fb, 0xFF, board_fb_size()); /* white, matches jw_ui clear() */

    /* board_fb_size() already returns one stripe's worth for a striped
     * board (docs/design/display-regions.md) - this bring-up test only
     * ever checks the first stripe, not the whole panel, on such a
     * board; fine for a sanity check, not exhaustive (flagged for
     * removal anyway, see cadran.h). */
    int stripe_h = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = 0, .height = stripe_h };
    err = cadran_render(face, &ctx);
    bool render_ok = (err == ESP_OK);

    /* Confirm the RECT and TEXT widgets actually flipped pixels inside
     * their footprint (only checked on 1bpp e-ink for now - watchy_v3 is
     * the only board so far). Text widget 3 sits at (5,5), scale 1
     * ("batt {v}%" is at least 4 chars wide -> footprint well inside a
     * generous 40px scan window). */
    bool rect_drew = true, text_drew = true;
    if (render_ok && b->caps.disp_kind == DISP_EINK_1BIT) {
        size_t stride = ((size_t)b->caps.disp_w + 7) / 8;
        rect_drew = false;
        for (int yy = 2; yy < 12 && !rect_drew; yy++) {
            for (int xx = 2; xx < 22; xx++) {
                size_t bi = (size_t)yy * stride + (size_t)xx / 8;
                uint8_t mask = 0x80 >> (xx % 8);
                if (bi < board_fb_size() && !(fb[bi] & mask)) { rect_drew = true; break; }
            }
        }
        text_drew = false;
        for (int yy = 5; yy < 13 && !text_drew; yy++) {
            for (int xx = 5; xx < 45; xx++) {
                size_t bi = (size_t)yy * stride + (size_t)xx / 8;
                uint8_t mask = 0x80 >> (xx % 8);
                if (bi < board_fb_size() && !(fb[bi] & mask)) { text_drew = true; break; }
            }
        }
    }

    bool pass = render_ok && rect_drew && text_drew;
    ESP_LOGI(TAG, "%s: load=ok strings=ok render=%s rect_pixels=%s text_pixels=%s",
             pass ? "PASS" : "FAIL",
             render_ok ? "ok" : "failed",
             rect_drew ? "drawn" : "MISSING",
             text_drew ? "drawn" : "MISSING");

#if CADRAN_SELFTEST_BLIT_TO_PANEL
    if (render_ok && b->display) {
        if (b->display->begin_frame) b->display->begin_frame();
        esp_err_t berr = b->display->blit_region(0, 0, b->caps.disp_w, stripe_h, fb);
        if (berr == ESP_OK) berr = b->display->end_frame(true /* full refresh */);
        ESP_LOGI(TAG, "blit to panel: %s", berr == ESP_OK ? "ok" : esp_err_to_name(berr));
    }
#endif

    free(fb);
    cadran_face_free(face);
}
