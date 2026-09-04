/**
 * jw.ui — reference native module.
 *
 * Registered per context only if the board has a display capability AND the
 * manifest doesn't need a permission for it (UI is always allowed). Serves
 * as the template for jw.net / jw.storage / jw.sensors, which additionally
 * check manifest permissions before being registered at all.
 *
 * Rendering model: apps draw into the canonical framebuffer via primitives;
 * the launcher blits + updates after JS_HOOK_ON_RENDER returns. E-ink and
 * AMOLED boards share the same API; disp_kind decides the fb format.
 */
#include <string.h>
#include "esp_log.h"
#include "board_hal/board.h"
#include "unruh/engine.h"
#include "quickjs.h"
#include "gfx/text.h"

static const char *TAG = "jw.ui";

/* Owned by the launcher, injected via jw_ui_bind_fb() - once per stripe
 * on a striped board (docs/design/display-regions.md), rebound between
 * onRender() calls without the app ever knowing: app code always uses
 * panel-absolute x/y, s_gfx_ctx's origin_y/height do the clipping. For a
 * non-striped board this is called once per frame, same as before
 * gfx_ctx_t existed. */
static gfx_ctx_t s_gfx_ctx;
static bool      s_dirty;

void jw_ui_bind_fb(const gfx_ctx_t *ctx) { s_gfx_ctx = *ctx; }
bool jw_ui_take_dirty(void) { bool d = s_dirty; s_dirty = false; return d; }

/* ------------------------------------------------------------ primitives */

static JSValue ui_clear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    /* Clears only the current stripe, not the whole panel - matches
     * board_fb_size() (one stripe's worth on a striped board) and is
     * exactly what a fresh onRender() call for this stripe should do:
     * memset the buffer it's actually about to draw into, nothing more. */
    if (s_gfx_ctx.fb) memset(s_gfx_ctx.fb, 0xFF, board_fb_size());   /* white on e-ink */
    s_dirty = true;
    return JS_UNDEFINED;
}

static JSValue ui_text(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t x = 0, y = 0, scale = 1;
    if (argc < 3) return JS_ThrowTypeError(ctx, "text(x, y, str, scale?)");
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    const char *s = JS_ToCString(ctx, argv[2]);
    if (!s) return JS_EXCEPTION;
    if (argc >= 4) JS_ToInt32(ctx, &scale, argv[3]);
    if (s_gfx_ctx.fb) gfx_draw_text(&s_gfx_ctx, (int)x, (int)y, s, (int)scale);
    ESP_LOGI(TAG, "text(%d,%d) x%d: %s", (int)x, (int)y, (int)scale, s);
    JS_FreeCString(ctx, s);
    s_dirty = true;
    return JS_UNDEFINED;
}

static JSValue ui_invalidate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv;
    s_dirty = true;
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------- install */

static int install(js_engine_t *e, void *ctx_opaque) {
    (void)e;
    JSContext *ctx = ctx_opaque;
    JSValue g  = JS_GetGlobalObject(ctx);
    JSValue jw = JS_GetPropertyStr(ctx, g, "jw");
    JSValue ui = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, ui, "clear",
        JS_NewCFunction(ctx, ui_clear, "clear", 0));
    JS_SetPropertyStr(ctx, ui, "text",
        JS_NewCFunction(ctx, ui_text, "text", 3));
    JS_SetPropertyStr(ctx, ui, "invalidate",
        JS_NewCFunction(ctx, ui_invalidate, "invalidate", 0));

    JS_SetPropertyStr(ctx, jw, "ui", ui);
    JS_FreeValue(ctx, jw);
    JS_FreeValue(ctx, g);
    return 0;
}

const js_module_def_t jw_ui_module = { .name = "jw_ui", .install = install };
