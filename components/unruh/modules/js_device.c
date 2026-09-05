/**
 * jw.device — the one write/actuator category in the first wave
 * (docs/design/js-api.md §2/§4): jw.device.Vibrator(), start()/stop()
 * only, no scene/intensity API yet (no board has a programmable haptic
 * driver, just a fixed-strength motor GPIO). Separate permission
 * category from jw.sensors on purpose - see app_store.h's perm_device
 * field, a deliberate divergence from Zepp's own docs nav (js-api.md §2).
 *
 * Safety ceiling lives here, not in board_hal (vibrator_ops_t stays a
 * plain motor on/off, board.h's own comment) and not left to app trust
 * (Jan's explicit go-ahead, project chat 2026-09-05: "Eine Maximaldauer
 * und ein Ende beim Abräumen der Engine gehören in den C-Code, nicht ins
 * Vertrauen auf die App."): a one-shot esp_timer auto-stops the motor
 * after KB_VIBRATOR_MAX_MS regardless of whether the app ever calls
 * stop(), and jw_device_force_stop() (called from launcher.c on every
 * engine teardown, not just a clean JS stop() call) guarantees the
 * motor can't keep running once the JS world that started it is gone.
 */
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "board_hal/board.h"
#include "unruh/engine.h"
#include "quickjs.h"

static const char *TAG = "jw.device";

/* Hard ceiling, not a default the app can extend - a stuck/runaway app
 * calling start() and never stop() can't hold the motor on past this,
 * full stop. 3s comfortably covers a deliberate multi-buzz UI pattern
 * without leaving room for a "forgot to call stop()" bug to matter. */
#define KB_VIBRATOR_MAX_MS 3000

static esp_timer_handle_t s_stop_timer;
static bool               s_active;

static void auto_stop_cb(void *arg) {
    (void)arg;
    const board_desc_t *b = board_get();
    if (b->vibrator->stop) b->vibrator->stop();
    s_active = false;
    ESP_LOGW(TAG, "auto-stop after %d ms (app never called stop())", KB_VIBRATOR_MAX_MS);
}

static void ensure_timer(void) {
    if (s_stop_timer) return;
    const esp_timer_create_args_t args = {
        .callback = auto_stop_cb,
        .name = "jw_device_vib",
    };
    esp_timer_create(&args, &s_stop_timer);
}

/* Called from launcher.c on every engine teardown (idle-timeout sleep,
 * explicit BACK, a Watchface's build()-then-destroy path) - not just
 * when the app itself called stop(). Safe to call with nothing running:
 * esp_timer_stop() on an already-inactive timer, board stop() only if
 * s_active - same "safe no-op" contract js_destroy(NULL) already has
 * (launcher.c's teardown_engine_if_running() comment). */
void jw_device_force_stop(void) {
    if (s_stop_timer) esp_timer_stop(s_stop_timer);
    if (s_active) {
        const board_desc_t *b = board_get();
        if (b->vibrator->stop) b->vibrator->stop();
        s_active = false;
    }
}

static JSValue vib_start(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv;
    const board_desc_t *b = board_get();
    if (!b->vibrator->start) {
        ESP_LOGW(TAG, "start(): no vibration motor on this board");
        return JS_UNDEFINED;
    }
    ensure_timer();
    b->vibrator->start();
    s_active = true;
    if (s_stop_timer) esp_timer_start_once(s_stop_timer, (uint64_t)KB_VIBRATOR_MAX_MS * 1000);
    return JS_UNDEFINED;
}

static JSValue vib_stop(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv;
    jw_device_force_stop();
    return JS_UNDEFINED;
}

static JSValue device_vibrator_factory(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "start", JS_NewCFunction(ctx, vib_start, "start", 0));
    JS_SetPropertyStr(ctx, o, "stop", JS_NewCFunction(ctx, vib_stop, "stop", 0));
    return o;
}

static int install(js_engine_t *e, void *ctx_opaque) {
    (void)e;
    JSContext *ctx = ctx_opaque;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue jw = JS_GetPropertyStr(ctx, g, "jw");
    JSValue device = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, device, "Vibrator",
        JS_NewCFunction(ctx, device_vibrator_factory, "Vibrator", 0));

    JS_SetPropertyStr(ctx, jw, "device", device);
    JS_FreeValue(ctx, jw);
    JS_FreeValue(ctx, g);
    return 0;
}

const js_module_def_t jw_device_module = { .name = "jw_device", .install = install };
