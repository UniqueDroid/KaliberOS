/**
 * jw.WatchFace — declarative-face serializer + registration.
 *
 * Design: docs/design/cadran-watchface-engine.md. WatchFace({build}).build()
 * runs once and returns a widget tree (design doc §3); this module
 * validates that tree and serializes it into face.bin bytes matching
 * cadran/include/cadran/face_format.h. Depends on Cadran's format header
 * only - Cadran itself has zero dependency back on unruh/quickjs, this
 * edge goes one way (same rationale as gfx, see components/gfx).
 *
 * Not wired into the launcher yet (design doc §8 / roadmap step 6): today
 * the only caller is js_watchface_selftest(), a bring-up harness that
 * proves the full round trip (build() -> serialize -> cadran_face_load()
 * -> cadran_render()) the way cadran_selftest() proved the loader/
 * renderer/provider round trip before this existed.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "board_hal/board.h"
#include "unruh/engine.h"
#include "cadran/cadran.h"
#include "cadran/face_format.h"
#include "quickjs.h"
#include "watchface_selftest_bytecode.h"

static const char *TAG = "jw.watchface";

/* Owned by the caller of install() (mirrors engine_quickjs.c's e->app_obj
 * for App(), and js_ui.c's jw_ui_bind_fb() stash-what-I-need pattern) -
 * this whole module assumes a single engine instance at a time, same as
 * the rest of this codebase. */
static JSContext *s_ctx;
static JSValue s_watchface_obj;

/* ---------------------------------------------------------- WatchFace() */

static JSValue js_global_WatchFace(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "WatchFace() expects a descriptor object");
    JS_FreeValue(ctx, s_watchface_obj);
    s_watchface_obj = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static int install(js_engine_t *e, void *ctx_opaque) {
    (void)e;
    s_ctx = ctx_opaque;
    JSValue g = JS_GetGlobalObject(s_ctx);
    JS_SetPropertyStr(s_ctx, g, "WatchFace",
        JS_NewCFunction(s_ctx, js_global_WatchFace, "WatchFace", 1));
    JS_FreeValue(s_ctx, g);
    s_watchface_obj = JS_UNDEFINED;
    return 0;
}

const js_module_def_t jw_watchface_module = { .name = "jw_watchface", .install = install };

/* ------------------------------------------------------------- helpers */

static esp_err_t get_int(JSContext *ctx, JSValueConst obj, const char *key,
                          int32_t *out, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v)) { *out = def; JS_FreeValue(ctx, v); return ESP_OK; }
    int r = JS_ToInt32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* Bump if a face grows beyond this - it's a structural sanity cap, not a
 * real target (design doc §6: "typical face < 1 kB"). */
#define WATCHFACE_MAX_STRINGS_LEN 4096

typedef struct {
    uint8_t buf[WATCHFACE_MAX_STRINGS_LEN];
    size_t  len;
} strtab_t;

/* Returns CADRAN_STR_NONE on overflow - 0 is a legitimate ref (the first
 * string interned lands at offset 0), so the sentinel is the only signal
 * a caller can check. */
static uint16_t strtab_add(strtab_t *st, const char *s) {
    size_t slen = strlen(s) + 1; /* include NUL - render.c/draw_text does
                                   * strstr/strncpy on this, needs it */
    if (st->len + slen > sizeof st->buf) return CADRAN_STR_NONE;
    uint16_t ref = (uint16_t)st->len;
    memcpy(st->buf + st->len, s, slen);
    st->len += slen;
    return ref;
}

typedef struct { const char *name; cadran_provider_id_t id; } provider_map_t;
static const provider_map_t PROVIDERS[] = {
    {"time.h",          CADRAN_PROVIDER_TIME_H},
    {"time.m",          CADRAN_PROVIDER_TIME_M},
    {"time.hm",         CADRAN_PROVIDER_TIME_HM},
    {"time.min_angle",  CADRAN_PROVIDER_TIME_MIN_ANGLE},
    {"time.hour_angle", CADRAN_PROVIDER_TIME_HOUR_ANGLE},
    {"date.d",          CADRAN_PROVIDER_DATE_D},
    {"date.m",          CADRAN_PROVIDER_DATE_M},
    {"date.wd",         CADRAN_PROVIDER_DATE_WD},
    {"battery.pct",     CADRAN_PROVIDER_BATTERY_PCT},
};

/* "app.0".."app.7" -> CADRAN_PROVIDER_APP_0+N, per design doc §5 - not
 * writable by anything yet (no hybrid-face JS API), but the name mapping
 * costs nothing to support now. */
static bool resolve_provider(const char *name, cadran_provider_id_t *out) {
    if (!name) return false;
    for (size_t i = 0; i < sizeof PROVIDERS / sizeof PROVIDERS[0]; i++) {
        if (strcmp(PROVIDERS[i].name, name) == 0) { *out = PROVIDERS[i].id; return true; }
    }
    if (strncmp(name, "app.", 4) == 0) {
        char *end;
        long n = strtol(name + 4, &end, 10);
        if (*end == '\0' && n >= 0 && n <= 7) {
            *out = (cadran_provider_id_t)(CADRAN_PROVIDER_APP_0 + (int)n);
            return true;
        }
    }
    return false;
}

typedef struct { const char *name; cadran_widget_type_t type; } type_map_t;
static const type_map_t TYPES[] = {
    {"img",        CADRAN_WIDGET_IMG},
    {"img_digits", CADRAN_WIDGET_IMG_DIGITS},
    {"text",       CADRAN_WIDGET_TEXT},
    {"arc",        CADRAN_WIDGET_ARC},
    {"img_level",  CADRAN_WIDGET_IMG_LEVEL},
    {"hand",       CADRAN_WIDGET_HAND},
    {"rect",       CADRAN_WIDGET_RECT},
    {"line",       CADRAN_WIDGET_LINE},
};

/* ---------------------------------------------------------- serializer */

/* Structural problems (bad shape, unknown widget type, too many widgets,
 * string table overflow) reject the whole build - the design doc gives
 * no degradation rule for those, unlike per-widget provider/bounds
 * issues (design doc §3: unavailable provider -> skip the widget, not an
 * error), which are just skipped and logged here. Widget count is capped
 * against the pre-filter array length, not the emitted count - a
 * pathological build() returning hundreds of widgets is rejected outright
 * rather than silently truncated. */
static esp_err_t serialize(JSContext *ctx, JSValueConst tree, const board_desc_t *board,
                            uint8_t **out_buf, size_t *out_len) {
    if (!JS_IsObject(tree)) {
        ESP_LOGE(TAG, "build() must return an object");
        return ESP_ERR_INVALID_ARG;
    }
    JSValue widgets_v = JS_GetPropertyStr(ctx, tree, "widgets");
    if (!JS_IsArray(widgets_v)) {
        ESP_LOGE(TAG, "build() result has no widgets array");
        JS_FreeValue(ctx, widgets_v);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t n_in = 0;
    {
        JSValue lv = JS_GetPropertyStr(ctx, widgets_v, "length");
        JS_ToUint32(ctx, &n_in, lv);
        JS_FreeValue(ctx, lv);
    }
    if (n_in > 255) {
        ESP_LOGE(TAG, "too many widgets: %u > 255 (u8 widget_count)", (unsigned)n_in);
        JS_FreeValue(ctx, widgets_v);
        return ESP_ERR_INVALID_SIZE;
    }

    strtab_t *st = calloc(1, sizeof *st); /* too big for the stack */
    cadran_widget_rec_t *recs = n_in ? calloc(n_in, sizeof *recs) : NULL;
    if (!st || (n_in && !recs)) {
        free(st); free(recs);
        JS_FreeValue(ctx, widgets_v);
        return ESP_ERR_NO_MEM;
    }
    uint8_t n_out = 0;
    esp_err_t fail = ESP_OK;

    for (uint32_t i = 0; i < n_in && fail == ESP_OK; i++) {
        JSValue w = JS_GetPropertyUint32(ctx, widgets_v, i);

        JSValue type_v = JS_GetPropertyStr(ctx, w, "type");
        const char *type_s = JS_ToCString(ctx, type_v);
        cadran_widget_type_t wtype = CADRAN_WIDGET_RECT; /* silence -Wmaybe-uninitialized */
        bool type_ok = false;
        if (type_s) {
            for (size_t k = 0; k < sizeof TYPES / sizeof TYPES[0]; k++) {
                if (strcmp(TYPES[k].name, type_s) == 0) { wtype = TYPES[k].type; type_ok = true; break; }
            }
        }
        if (!type_ok) {
            ESP_LOGE(TAG, "widget %u: unknown type '%s'", (unsigned)i, type_s ? type_s : "?");
            fail = ESP_ERR_INVALID_ARG;
        }
        JS_FreeCString(ctx, type_s);
        JS_FreeValue(ctx, type_v);
        if (fail != ESP_OK) { JS_FreeValue(ctx, w); break; }

        int32_t x = 0, y = 0;
        get_int(ctx, w, "x", &x, 0);
        get_int(ctx, w, "y", &y, 0);
        if (x < 0 || y < 0 || x >= board->caps.disp_w || y >= board->caps.disp_h) {
            ESP_LOGW(TAG, "widget %u: (%ld,%ld) outside panel %dx%d, skipped",
                     (unsigned)i, (long)x, (long)y, board->caps.disp_w, board->caps.disp_h);
            JS_FreeValue(ctx, w);
            continue;
        }

        cadran_widget_rec_t rec = {0};
        rec.type = (uint8_t)wtype;
        rec.x = (int16_t)x;
        rec.y = (int16_t)y;
        rec.bind_id = CADRAN_PROVIDER_NONE;
        rec.str_ref = CADRAN_STR_NONE;

        JSValue bind_v = JS_GetPropertyStr(ctx, w, "bind");
        bool skip_unbound = false;
        if (!JS_IsUndefined(bind_v)) {
            const char *bind_s = JS_ToCString(ctx, bind_v);
            cadran_provider_id_t pid;
            if (bind_s && resolve_provider(bind_s, &pid)) {
                rec.bind_id = (uint8_t)pid;
            } else {
                /* Design doc §3's degradation rule: unresolvable bind ->
                 * skip the widget, not a build error. */
                ESP_LOGW(TAG, "widget %u: unknown bind '%s', skipped",
                         (unsigned)i, bind_s ? bind_s : "?");
                skip_unbound = true;
            }
            JS_FreeCString(ctx, bind_s);
        }
        JS_FreeValue(ctx, bind_v);
        if (skip_unbound) { JS_FreeValue(ctx, w); continue; }

        switch (wtype) {
        case CADRAN_WIDGET_RECT: {
            int32_t rw, rh, filled;
            get_int(ctx, w, "w", &rw, 0);
            get_int(ctx, w, "h", &rh, 0);
            get_int(ctx, w, "filled", &filled, 0);
            rec.params[0] = (int16_t)rw; rec.params[1] = (int16_t)rh;
            rec.params[2] = (int16_t)(filled ? 1 : 0);
            break;
        }
        case CADRAN_WIDGET_LINE: {
            int32_t x2, y2;
            get_int(ctx, w, "x2", &x2, 0);
            get_int(ctx, w, "y2", &y2, 0);
            rec.params[0] = (int16_t)x2; rec.params[1] = (int16_t)y2;
            break;
        }
        case CADRAN_WIDGET_HAND: {
            int32_t len;
            get_int(ctx, w, "len", &len, 0);
            rec.params[0] = (int16_t)len;
            break;
        }
        case CADRAN_WIDGET_TEXT: {
            int32_t scale;
            get_int(ctx, w, "scale", &scale, 0);
            rec.params[0] = (int16_t)scale;
            JSValue fmt_v = JS_GetPropertyStr(ctx, w, "format");
            const char *fmt_s = JS_ToCString(ctx, fmt_v);
            if (fmt_s) {
                uint16_t ref = strtab_add(st, fmt_s);
                if (ref == CADRAN_STR_NONE) {
                    ESP_LOGE(TAG, "widget %u: string table overflow (>%d bytes)",
                             (unsigned)i, WATCHFACE_MAX_STRINGS_LEN);
                    fail = ESP_ERR_INVALID_SIZE;
                } else {
                    rec.str_ref = ref;
                }
            }
            JS_FreeCString(ctx, fmt_s);
            JS_FreeValue(ctx, fmt_v);
            break;
        }
        default:
            /* img/img_digits/arc/img_level: type recorded, x/y kept, but
             * no atelier resource pipeline exists yet to give these
             * meaningful params - cadran/render.c already skips them at
             * render time regardless of what's serialized here. */
            break;
        }

        if (fail == ESP_OK) recs[n_out++] = rec;
        JS_FreeValue(ctx, w);
    }
    JS_FreeValue(ctx, widgets_v);

    if (fail != ESP_OK) { free(st); free(recs); return fail; }

    cadran_header_t hdr = {
        .magic = {'C', 'D', 'R', 'N'}, .abi = CADRAN_ABI,
        .widget_count = n_out, .flags = 0,
    };
    size_t strings_len = st->len;
    size_t total = sizeof hdr + strings_len + (size_t)n_out * sizeof(cadran_widget_rec_t);
    uint8_t *buf = malloc(total);
    if (!buf) { free(st); free(recs); return ESP_ERR_NO_MEM; }
    uint8_t *p = buf;
    memcpy(p, &hdr, sizeof hdr);               p += sizeof hdr;
    memcpy(p, st->buf, strings_len);           p += strings_len;
    memcpy(p, recs, (size_t)n_out * sizeof(cadran_widget_rec_t));
    free(st);
    free(recs);

    ESP_LOGI(TAG, "serialized: %u/%u widgets, %u bytes strings, %u bytes total",
             n_out, (unsigned)n_in, (unsigned)strings_len, (unsigned)total);
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

/* --------------------------------------------------------------- build */

/* True once a loaded app has called the global WatchFace({...}) -
 * launcher.c's counterpart to js_has_app(), same "which lifecycle global
 * did this bytecode actually call" check js_load_app()'s own comment
 * describes, just for the declarative side instead of App(). */
bool js_watchface_has_face(void) {
    return JS_IsObject(s_watchface_obj);
}

/* Drops the held WatchFace({...}) descriptor - same cleanup
 * js_watchface_selftest() does before js_destroy(), now available to any
 * caller (the launcher) instead of being inlined there once only. Safe
 * to call even if WatchFace() was never registered (s_watchface_obj is
 * JS_UNDEFINED then, JS_FreeValue on it is a no-op). Call before
 * js_destroy(), not after - s_ctx must still be valid. */
void js_watchface_reset(void) {
    if (s_ctx) JS_FreeValue(s_ctx, s_watchface_obj);
    s_watchface_obj = JS_UNDEFINED;
}

/* Runs the registered WatchFace({build}).build(ctx) hook and serializes
 * its return value. ctx: {w, h, disp: "eink1"|"rgb565", caps: []} per
 * design doc §3 - caps is always empty for now (steps/hr/stress aren't
 * gated/exposed anywhere yet). Public (not the selftest's own private
 * helper anymore, project chat 2026-09-05): the launcher calls this too,
 * now that step 6 (design doc §8/§9) wires Cadran in for real. */
esp_err_t js_watchface_build(const board_desc_t *board, uint8_t **out_buf, size_t *out_len) {
    if (!s_ctx || !JS_IsObject(s_watchface_obj)) {
        ESP_LOGE(TAG, "no WatchFace({...}) registered");
        return ESP_ERR_INVALID_STATE;
    }
    JSValue build_fn = JS_GetPropertyStr(s_ctx, s_watchface_obj, "build");
    if (!JS_IsFunction(s_ctx, build_fn)) {
        ESP_LOGE(TAG, "WatchFace descriptor has no build()");
        JS_FreeValue(s_ctx, build_fn);
        return ESP_ERR_INVALID_ARG;
    }

    JSValue jctx = JS_NewObject(s_ctx);
    JS_SetPropertyStr(s_ctx, jctx, "w", JS_NewInt32(s_ctx, board->caps.disp_w));
    JS_SetPropertyStr(s_ctx, jctx, "h", JS_NewInt32(s_ctx, board->caps.disp_h));
    JS_SetPropertyStr(s_ctx, jctx, "disp", JS_NewString(s_ctx,
        board->caps.disp_kind == DISP_EINK_1BIT ? "eink1" : "rgb565"));
    JS_SetPropertyStr(s_ctx, jctx, "caps", JS_NewArray(s_ctx));

    JSValue tree = JS_Call(s_ctx, build_fn, s_watchface_obj, 1, &jctx);
    JS_FreeValue(s_ctx, build_fn);
    JS_FreeValue(s_ctx, jctx);

    if (JS_IsException(tree)) {
        JSValue x = JS_GetException(s_ctx);
        const char *msg = JS_ToCString(s_ctx, x);
        ESP_LOGE(TAG, "build() threw: %s", msg ? msg : "?");
        JS_FreeCString(s_ctx, msg);
        JS_FreeValue(s_ctx, x);
        JS_FreeValue(s_ctx, tree);
        return ESP_FAIL;
    }

    esp_err_t err = serialize(s_ctx, tree, board, out_buf, out_len);
    JS_FreeValue(s_ctx, tree);
    return err;
}

/* ---------------------------------------------------------- self-test */

/* Bring-up harness for the full round trip: engine up -> WatchFace()
 * registered by examples/watchfaces/simple/face.js -> build() -> serialize
 * -> engine down -> cadran_face_load() -> cadran_render(). Proves the
 * design's core promise (design doc §1) end to end: a real face authored
 * in JS renders through the C-only Cadran path with the engine already
 * gone. Temporary, same as cadran_selftest() - remove both once Cadran is
 * wired into the launcher for real (roadmap step 6). */
void js_watchface_selftest(void) {
    const board_desc_t *b = board_get();

    js_limits_t lim = {
        .heap_limit     = b->caps.js_heap_budget,
        .stack_limit    = b->caps.js_task_stack - 12 * 1024,
        .hook_budget_ms = 500,
    };
    js_engine_t *e = js_create(&lim);
    if (!e) { ESP_LOGE(TAG, "FAIL: js_create"); return; }

    if (js_register_module(e, &jw_watchface_module) != JS_OK) {
        ESP_LOGE(TAG, "FAIL: register jw_watchface");
        js_destroy(e);
        return;
    }
    if (js_load_app(e, k_face_qjb, sizeof k_face_qjb) != JS_OK) {
        ESP_LOGE(TAG, "FAIL: load face.js: %s", js_last_error(e));
        js_destroy(e);
        return;
    }

    uint8_t *face_buf = NULL;
    size_t face_len = 0;
    esp_err_t err = js_watchface_build(b, &face_buf, &face_len);

    /* Drop the held reference before the context/runtime go away - it's a
     * real DupValue (see js_global_WatchFace), not releasing it left a
     * GC object alive past JS_FreeContext and tripped JS_FreeRuntime's own
     * consistency assert (list_empty(&rt->gc_obj_list)) the first time
     * this ran. Then tear the engine down before touching Cadran - the
     * whole point of the design is that the tick path never needs it
     * (design doc §1); proving that means not leaning on it a moment
     * longer than build(). */
    js_watchface_reset();
    js_destroy(e);
    s_ctx = NULL;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: build/serialize: %s", esp_err_to_name(err));
        return;
    }

    /* Success criterion 2 (README): the real minute-tick path never boots
     * the engine at all - face.bin already exists on flash by then. Timed
     * and heap-measured right here, after js_destroy() and before
     * anything else runs, so both numbers reflect that path in isolation,
     * not this harness's own build()/serialize() cost above. */
    size_t heap_no_engine = esp_get_free_heap_size();
    int64_t t0 = esp_timer_get_time();

    cadran_face_t *face = NULL;
    err = cadran_face_load(face_buf, face_len, &face);
    free(face_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: cadran_face_load: %s", esp_err_to_name(err));
        return;
    }

    uint8_t *fb = calloc(1, board_fb_size());
    if (!fb) {
        ESP_LOGE(TAG, "FAIL: no mem for scratch fb");
        cadran_face_free(face);
        return;
    }
    memset(fb, 0xFF, board_fb_size());
    /* board_fb_size() returns one stripe's worth on a striped board -
     * this bring-up test only ever checks the first stripe (see
     * cadran/selftest.c's identical note). */
    int stripe_h = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    gfx_ctx_t wf_ctx = { .fb = fb, .board = b, .origin_y = 0, .height = stripe_h };
    err = cadran_render(face, &wf_ctx);
    bool render_ok = (err == ESP_OK);
    int64_t render_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "C-path (no engine): load+render %lld us, free heap %u B",
             (long long)render_us, (unsigned)heap_no_engine);

    bool text_drew = true;
    if (render_ok && b->caps.disp_kind == DISP_EINK_1BIT) {
        text_drew = false;
        size_t stride = ((size_t)b->caps.disp_w + 7) / 8;
        for (int yy = 80; yy < 88 && !text_drew; yy++) {
            for (int xx = 20; xx < 100; xx++) {
                size_t bi = (size_t)yy * stride + (size_t)xx / 8;
                uint8_t mask = 0x80 >> (xx % 8);
                if (bi < board_fb_size() && !(fb[bi] & mask)) { text_drew = true; break; }
            }
        }
    }

    bool pass = render_ok && text_drew;
    ESP_LOGI(TAG, "%s: build=ok serialize=ok(%u B) load=ok render=%s text_pixels=%s",
             pass ? "PASS" : "FAIL", (unsigned)face_len,
             render_ok ? "ok" : "failed", text_drew ? "drawn" : "MISSING");

#if 0 /* flip to 1 for a hardware verification pass, same as
       * CADRAN_SELFTEST_BLIT_TO_PANEL in cadran/selftest.c */
    if (render_ok && b->display) {
        if (b->display->begin_frame) b->display->begin_frame();
        esp_err_t berr = b->display->blit_region(0, 0, b->caps.disp_w, stripe_h, fb);
        if (berr == ESP_OK) berr = b->display->end_frame(true);
        ESP_LOGI(TAG, "blit to panel: %s", berr == ESP_OK ? "ok" : esp_err_to_name(berr));
    }
#endif

    free(fb);
    cadran_face_free(face);
}
