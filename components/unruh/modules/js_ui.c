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

static const char *TAG = "jw.ui";

/* Owned by the launcher, injected via jw_ui_bind_fb(). */
static uint8_t *s_fb;
static bool     s_dirty;

void jw_ui_bind_fb(uint8_t *fb) { s_fb = fb; }
bool jw_ui_take_dirty(void) { bool d = s_dirty; s_dirty = false; return d; }

/* ------------------------------------------------------------ primitives */

static JSValue ui_clear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (s_fb) memset(s_fb, 0xFF, board_fb_size());   /* white on e-ink */
    s_dirty = true;
    return JS_UNDEFINED;
}

static JSValue ui_text(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t x = 0, y = 0;
    if (argc < 3) return JS_ThrowTypeError(ctx, "text(x, y, str)");
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    const char *s = JS_ToCString(ctx, argv[2]);
    if (!s) return JS_EXCEPTION;
    /* TODO: rasterize into s_fb with the built-in font.
     * For bring-up, log instead of drawing: */
    ESP_LOGI(TAG, "text(%d,%d): %s", (int)x, (int)y, s);
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
