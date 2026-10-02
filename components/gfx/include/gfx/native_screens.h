/**
 * Kaliber gfx — native-screen layout/content, shared by launcher.c and
 * net_svc.c (and, on the host, tools/host_render).
 *
 * Extracted (2026-10-02) because launcher.c and net_svc.c had each grown
 * their own copy of screen_margin()/screen_line_gap()/headline-or-detail
 * font selection - small helpers, deliberately duplicated per this
 * project's own convention (see gfx/text.c's set_px comment) - but the
 * *content* composition (which strings go where) was duplicated too, with
 * no way to render it outside real hardware. That's what made the last
 * few rounds of "flash, photograph, guess" necessary instead of catching
 * layout bugs locally. This file has zero ESP-IDF/FreeRTOS dependency
 * (gfx + board_hal only, same as gfx/text.c) specifically so it can be
 * linked into a host-side test binary - callers (launcher.c, net_svc.c)
 * still own the stripe loop, begin_frame/blit_region/end_frame and
 * fb allocation, since those are hardware/display concerns, not layout.
 */
#pragma once

#include "board_hal/board.h"
#include "gfx/text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ~2.5% of panel width, same ratio smartwatch-system/'s own margin (24/390)
 * uses - floor of 4px so a very narrow hypothetical panel still has some
 * breathing room. */
int gfx_screen_margin(const board_desc_t *b);

/* Headline y-offset for the launcher's own screens (NO APPS/MENU/WAKE
 * CHECK) - net_svc's sync screen intentionally starts its headline at
 * the margin itself instead (see gfx_screen_margin()'s callers), kept as
 * a separate function rather than folded in, since the two screens use
 * different vertical rhythm on purpose. */
int gfx_screen_headline_y(const board_desc_t *b);

/* Line pitch for stacked detail lines below a headline. */
int gfx_screen_line_gap(const board_desc_t *b);

/* Biggest of gfx_font_32/gfx_font_16 that still fits `str` inside this
 * board's own disp_w at the given margin - same fit-check shape as
 * cadran/render.c's resolve_font(), one tier shallower (no "large" role
 * here, headlines never need digit-only sizing). */
const gfx_font_t *gfx_headline_font(const board_desc_t *b, const char *str, int margin);

/* gfx_font_16 if the widest line this family of screens ever draws ("KEY:  "
 * + 16 hex chars, 22 chars) fits this board's disp_w, else NULL - NULL
 * means "fall back to the built-in 8x8 font via gfx_draw_text(), not
 * gfx_draw_text_font()" (the 8x8 font isn't a gfx_font_t). Pass the
 * result to gfx_draw_detail_line(). */
const gfx_font_t *gfx_detail_font(const board_desc_t *b, int margin);

/* Draws one already-formatted line with detail_font()'s result (or the
 * built-in 8x8 font if NULL) at scale 1. */
void gfx_draw_detail_line(const gfx_ctx_t *ctx, int x, int y, const char *s,
                           const gfx_font_t *font);

/* ---------------------------------------------------- screen content */
/* Each function draws into the given stripe's ctx only - callers still
 * own memset-to-white, the stripe loop itself, and the hardware blit.
 * Panel-absolute x/y throughout, same convention as every other gfx
 * call - a widget/line that straddles this stripe is clipped, not
 * skipped, same mechanism as everywhere else. */

void gfx_screens_draw_no_apps(const gfx_ctx_t *ctx);
void gfx_screens_draw_menu(const gfx_ctx_t *ctx);
void gfx_screens_draw_wake_check(const gfx_ctx_t *ctx, const char *msg);

/* The "APP: <id>" state-label overlay drawn on top of a running app's own
 * content (launcher.c's app_render_if_dirty()) - was hardcoded to (10,190),
 * sized for watchy_v3's 200x200 panel only (found 2026-10-02, same bug
 * class as the screens above had before the 2026-09-07 relative-layout
 * fix, just missed that round because it lives in a different function).
 * Now anchored bottom-left, relative to this board's own panel. */
void gfx_screens_draw_app_label(const gfx_ctx_t *ctx, const char *app_id);

/* net_svc's sync-mode screen: SYNC headline + SSID/PASS/IP/KEY detail
 * lines (KEY wrapped 16 chars/line across 4 lines - 64 hex chars never
 * fits one line on either board). */
void gfx_screens_draw_sync(const gfx_ctx_t *ctx, const char *ssid,
                            const char *pass, const char *ip, const char *key);

#ifdef __cplusplus
}
#endif
