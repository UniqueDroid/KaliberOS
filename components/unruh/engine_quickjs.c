/**
 * Unruh backend: QuickJS.
 *
 * Requires the quickjs component (see components/unruh/CMakeLists.txt and
 * README "Engine einbinden"). Heap goes to PSRAM when available; hard
 * memory limit and a wall-clock interrupt handler make hostile apps fail
 * as JS errors instead of watchdog resets.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "hal/board.h"
#include "unruh/engine.h"
#include "quickjs.h"

static const char *TAG = "unruh.qjs";

struct js_engine {
    JSRuntime  *rt;
    JSContext  *ctx;
    JSValue     app_obj;          /* object passed to global App({...})     */
    js_limits_t lim;
    int64_t     hook_deadline_us; /* 0 = no budget active                   */
    char        err[160];
    /* TODO: timer wheel (id -> {due_us, JSValue cb, interval}) */
};

/* ------------------------------------------------------- allocator hooks */

static void *psram_calloc(JSMallocState *s, size_t count, size_t size) {
    (void)s;
    return heap_caps_calloc(count, size,
        board_get()->caps.has_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
}
static void *psram_malloc(JSMallocState *s, size_t size) {
    (void)s;
    return heap_caps_malloc(size,
        board_get()->caps.has_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
}
static void psram_free(JSMallocState *s, void *ptr) { (void)s; heap_caps_free(ptr); }
static void *psram_realloc(JSMallocState *s, void *ptr, size_t size) {
    (void)s;
    return heap_caps_realloc(ptr, size,
        board_get()->caps.has_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
}

static const JSMallocFunctions kb_malloc_fns = {
    .js_calloc  = psram_calloc,
    .js_malloc  = psram_malloc,
    .js_free    = psram_free,
    .js_realloc = psram_realloc,
    /* js_malloc_usable_size: leave NULL, QuickJS falls back safely */
};

/* --------------------------------------------------- interrupt (budget) */

static int interrupt_handler(JSRuntime *rt, void *opaque) {
    js_engine_t *e = opaque;
    (void)rt;
    if (e->hook_deadline_us && esp_timer_get_time() > e->hook_deadline_us) {
        snprintf(e->err, sizeof e->err, "hook exceeded %lu ms budget",
                 (unsigned long)e->lim.hook_budget_ms);
        return 1; /* abort execution -> JS_ERR_TIMEOUT */
    }
    return 0;
}

/* ------------------------------------------------------- global App(...) */

static JSValue js_global_App(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    js_engine_t *e = JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "App() expects a lifecycle object");
    JS_FreeValue(ctx, e->app_obj);
    e->app_obj = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------- API */

js_engine_t *js_create(const js_limits_t *lim) {
    js_engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->lim = *lim;

    e->rt = JS_NewRuntime2(&kb_malloc_fns, NULL);
    if (!e->rt) { free(e); return NULL; }

    JS_SetMemoryLimit(e->rt, lim->heap_limit);
    JS_SetMaxStackSize(e->rt, lim->stack_limit);
    JS_SetInterruptHandler(e->rt, interrupt_handler, e);

    e->ctx = JS_NewContext(e->rt);
    if (!e->ctx) { JS_FreeRuntime(e->rt); free(e); return NULL; }
    JS_SetContextOpaque(e->ctx, e);
    e->app_obj = JS_UNDEFINED;

    JSValue g = JS_GetGlobalObject(e->ctx);
    JS_SetPropertyStr(e->ctx, g, "App",
        JS_NewCFunction(e->ctx, js_global_App, "App", 1));
    /* "jw" namespace object; modules attach below it */
    JS_SetPropertyStr(e->ctx, g, "jw", JS_NewObject(e->ctx));
    JS_FreeValue(e->ctx, g);

    ESP_LOGI(TAG, "runtime up, heap budget %u kB",
             (unsigned)(lim->heap_limit / 1024));
    return e;
}

void js_destroy(js_engine_t *e) {
    if (!e) return;
    JS_FreeValue(e->ctx, e->app_obj);
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    free(e);
}

js_status_t js_register_module(js_engine_t *e, const js_module_def_t *def) {
    return def->install(e, e->ctx) == 0 ? JS_OK : JS_ERR_INTERNAL;
}

js_status_t js_load_app(js_engine_t *e, const uint8_t *bytecode, size_t len) {
    JSValue obj = JS_ReadObject(e->ctx, bytecode, len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) goto exc;
    JSValue ret = JS_EvalFunction(e->ctx, obj);  /* consumes obj */
    if (JS_IsException(ret)) goto exc;
    JS_FreeValue(e->ctx, ret);
    if (!JS_IsObject(e->app_obj)) {
        snprintf(e->err, sizeof e->err, "app never called App({...})");
        return JS_ERR_BYTECODE;
    }
    return JS_OK;
exc: {
        JSValue x = JS_GetException(e->ctx);
        const char *s = JS_ToCString(e->ctx, x);
        snprintf(e->err, sizeof e->err, "%s", s ? s : "load failed");
        JS_FreeCString(e->ctx, s);
        JS_FreeValue(e->ctx, x);
        return JS_ERR_BYTECODE;
    }
}

static const char *hook_name(js_hook_t h) {
    switch (h) {
    case JS_HOOK_ON_INIT:    return "onInit";
    case JS_HOOK_ON_RESUME:  return "onResume";
    case JS_HOOK_ON_EVENT:   return "onEvent";
    case JS_HOOK_ON_RENDER:  return "onRender";
    case JS_HOOK_ON_SUSPEND: return "onSuspend";
    }
    return "?";
}

js_status_t js_call_hook(js_engine_t *e, js_hook_t hook,
                         const char *json_arg, char **out_json) {
    if (out_json) *out_json = NULL;
    JSValue fn = JS_GetPropertyStr(e->ctx, e->app_obj, hook_name(hook));
    if (!JS_IsFunction(e->ctx, fn)) {         /* optional hooks are fine */
        JS_FreeValue(e->ctx, fn);
        return JS_OK;
    }

    JSValue arg = JS_UNDEFINED;
    if (json_arg) {
        arg = JS_ParseJSON(e->ctx, json_arg, strlen(json_arg), "<arg>");
        if (JS_IsException(arg)) { JS_FreeValue(e->ctx, fn); goto exc; }
    }

    e->hook_deadline_us = esp_timer_get_time() +
                          (int64_t)e->lim.hook_budget_ms * 1000;
    JSValue ret = JS_Call(e->ctx, fn, e->app_obj, json_arg ? 1 : 0, &arg);
    e->hook_deadline_us = 0;

    JS_FreeValue(e->ctx, fn);
    JS_FreeValue(e->ctx, arg);
    if (JS_IsException(ret)) goto exc;

    if (hook == JS_HOOK_ON_SUSPEND && out_json && !JS_IsUndefined(ret)) {
        JSValue js = JS_JSONStringify(e->ctx, ret, JS_UNDEFINED, JS_UNDEFINED);
        const char *s = JS_ToCString(e->ctx, js);
        if (s) *out_json = strdup(s);
        JS_FreeCString(e->ctx, s);
        JS_FreeValue(e->ctx, js);
    }
    JS_FreeValue(e->ctx, ret);
    return JS_OK;

exc: {
        JSValue x = JS_GetException(e->ctx);
        const char *s = JS_ToCString(e->ctx, x);
        js_status_t st = (s && strstr(s, "budget")) ? JS_ERR_TIMEOUT
                                                    : JS_ERR_EXCEPTION;
        snprintf(e->err, sizeof e->err, "%s", s ? s : "exception");
        JS_FreeCString(e->ctx, s);
        JS_FreeValue(e->ctx, x);
        ESP_LOGW(TAG, "%s failed: %s", hook_name(hook), e->err);
        return st;
    }
}

js_status_t js_pump_jobs(js_engine_t *e) {
    JSContext *pctx;
    int r;
    while ((r = JS_ExecutePendingJob(e->rt, &pctx)) > 0) {}
    return r < 0 ? JS_ERR_EXCEPTION : JS_OK;
}

/* Timer wheel: TODO — keep a sorted list of {id, due, cb, interval}, feed
 * kb_bus receive timeout from js_next_timer_ms(). Registered from the
 * jw.sys module (setTimeout/setInterval). */
uint32_t    js_next_timer_ms(js_engine_t *e) { (void)e; return UINT32_MAX; }
js_status_t js_dispatch_timers(js_engine_t *e) { (void)e; return JS_OK; }

const char *js_last_error(js_engine_t *e) { return e->err; }

size_t js_mem_used(js_engine_t *e) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(e->rt, &u);
    return (size_t)u.memory_used_size;
}
