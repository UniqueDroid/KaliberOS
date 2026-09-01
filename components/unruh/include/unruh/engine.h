/**
 * Unruh — the balance wheel. Engine-agnostic JS runtime layer.
 *
 * Backends: engine_quickjs.c (full ES2020, needs PSRAM budget) and
 * engine_mqjs.c (MQuickJS, ES5 subset, ~10 kB RAM). Which one is compiled
 * in is decided by the board (caps.engine) via Kconfig.
 *
 * Threading contract: every function here must be called from js_task only.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_engine js_engine_t;

typedef enum {
    JS_HOOK_ON_INIT,     /* arg: NULL                                       */
    JS_HOOK_ON_RESUME,   /* arg: state JSON or NULL                         */
    JS_HOOK_ON_EVENT,    /* arg: event JSON {type, id?, tag?, body?}        */
    JS_HOOK_ON_RENDER,   /* arg: NULL (render via jw.ui module)             */
    JS_HOOK_ON_SUSPEND,  /* returns state JSON via out param                */
} js_hook_t;

typedef struct {
    size_t   heap_limit;      /* hard JS_SetMemoryLimit equivalent          */
    size_t   stack_limit;     /* engine-internal stack guard                */
    uint32_t hook_budget_ms;  /* wall-clock budget per hook call            */
} js_limits_t;

/* Native module definition: name + registration callback that installs
 * functions on the module object. ctx_opaque is the backend context. */
typedef struct {
    const char *name;                       /* e.g. "jw_ui"                 */
    int (*install)(js_engine_t *e, void *ctx_opaque);
} js_module_def_t;

typedef enum {
    JS_OK = 0,
    JS_ERR_OOM,
    JS_ERR_TIMEOUT,     /* hook exceeded budget, app must be killed         */
    JS_ERR_EXCEPTION,   /* JS exception; message via js_last_error()        */
    JS_ERR_BYTECODE,    /* version/ABI mismatch or corrupt                  */
    JS_ERR_INTERNAL,
} js_status_t;

js_engine_t *js_create(const js_limits_t *lim);
void         js_destroy(js_engine_t *e);

/* Register a native module before loading the app. */
js_status_t  js_register_module(js_engine_t *e, const js_module_def_t *def);

/* Load and evaluate app bytecode. The app calls the global App({...}),
 * which the backend provides, storing the lifecycle object. */
js_status_t  js_load_app(js_engine_t *e, const uint8_t *bytecode, size_t len);

/* Invoke a lifecycle hook. json_arg may be NULL. For ON_SUSPEND, *out_json
 * receives a malloc'd state string (caller frees), else out_json is NULL. */
js_status_t  js_call_hook(js_engine_t *e, js_hook_t hook,
                          const char *json_arg, char **out_json);

/* Drain pending promise jobs (QuickJS) / no-op (MQuickJS). Call after every
 * hook and after timer dispatch. */
js_status_t  js_pump_jobs(js_engine_t *e);

/* Next due JS timer in ms from now, or UINT32_MAX if none. Used by js_task
 * as the bus receive timeout. */
uint32_t     js_next_timer_ms(js_engine_t *e);
js_status_t  js_dispatch_timers(js_engine_t *e);

const char  *js_last_error(js_engine_t *e);

/* Memory currently used by the JS world (diagnostics). */
size_t       js_mem_used(js_engine_t *e);

#ifdef __cplusplus
}
#endif
