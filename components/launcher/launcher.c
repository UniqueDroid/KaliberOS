/**
 * Kaliber launcher — owns the js_task, the engine instance and the app
 * lifecycle. This is the only place that calls into Unruh.
 *
 * Wake flow (deep model):
 *   wake -> engine up -> load current app bytecode -> onInit/onResume(state)
 *   -> dispatch wake event -> onRender -> blit -> idle loop -> EV_IDLE_TIMEOUT
 *   -> onSuspend -> persist state -> deep sleep.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_hal/board.h"
#include "core/event_bus.h"
#include "core/power_mgr.h"
#include "core/app_store.h"
#include "unruh/engine.h"
#include "launcher/launcher.h"
#include "gfx/text.h"

static const char *TAG = "launcher";

extern const js_module_def_t jw_ui_module;
extern void jw_ui_bind_fb(const gfx_ctx_t *ctx);
extern bool jw_ui_take_dirty(void);

/* Same budget js_call_hook() (unruh/engine_quickjs.c) gives a single
 * onRender() - shared here so the launcher's per-stripe render pass
 * (below) enforces the *same* half-second across all stripes combined,
 * not half a second *per stripe* (16 stripes x 500 ms would be an 8 s
 * worst case, see docs/design/display-regions.md §6). Kept as its own
 * constant, not read out of js_engine_t (opaque to the launcher) - the
 * two budgets are conceptually the same number by design, not coupled
 * by a shared variable. */
#define KB_RENDER_PASS_BUDGET_MS 500

typedef struct {
    js_engine_t  *eng;
    kb_manifest_t mf;
    uint8_t      *fb;
    char          app_id[KB_APP_ID_MAX];
    bool          app_ok;
} launcher_t;

static launcher_t L;

/* --------------------------------------------------------- app handling */

static void app_fail(const char *why) {
    ESP_LOGE(TAG, "app '%s' failed: %s (%s)", L.app_id, why,
             L.eng ? js_last_error(L.eng) : "-");
    L.app_ok = false;
    /* TODO: draw error screen into fb + update, fall back to launcher menu */
}

static bool app_boot(const char *id) {
    const board_desc_t *b = board_get();

    if (kb_store_read_manifest(id, &L.mf) != ESP_OK) return false;
    if (L.mf.abi != KB_APP_ABI_VERSION) {
        ESP_LOGE(TAG, "ABI mismatch: app %lu, fw %d",
                 (unsigned long)L.mf.abi, KB_APP_ABI_VERSION);
        return false;
    }

    js_limits_t lim = {
        .heap_limit     = b->caps.js_heap_budget,
        /* Must stay well below caps.js_task_stack (the real FreeRTOS
         * task stack, e.g. 32k on watchy_v3): QuickJS's own guard trips
         * a JS RangeError, but if it's set higher than the actual task
         * stack, the FreeRTOS stack guard trips first instead - a hard
         * crash, not a catchable JS error. Was hardcoded to 64k here
         * while js_task_stack was 32k - always past the real limit. */
        .stack_limit    = b->caps.js_task_stack - 12 * 1024,
        .hook_budget_ms = 500,
    };
    L.eng = js_create(&lim);
    if (!L.eng) return false;
    /* Success criterion 1 (README): free heap after engine init, critical
     * on v2/RAM-constrained boards - log it every boot, not just once. */
    ESP_LOGI(TAG, "post-init: free heap %u B, engine using %u B",
             (unsigned)esp_get_free_heap_size(), (unsigned)js_mem_used(L.eng));

    /* Capability + permission gated module registration: a module the app
     * may not use simply does not exist in its context. */
    js_register_module(L.eng, &jw_ui_module);
    /* if (L.mf.perm_net)     js_register_module(L.eng, &jw_net_module);   */
    /* if (L.mf.perm_storage) js_register_module(L.eng, &jw_storage_module); */

    uint8_t *bc; size_t bclen;
    if (kb_store_read_bytecode(id, &bc, &bclen) != ESP_OK) return false;
    js_status_t st = js_load_app(L.eng, bc, bclen);
    free(bc);
    if (st != JS_OK) { app_fail("load"); return false; }
    /* js_load_app() itself no longer requires App({...}) - it's shared
     * with WatchFace() bytecode (js_watchface.c) - so the launcher's own
     * App()-only contract is checked here instead. */
    if (!js_has_app(L.eng)) {
        ESP_LOGE(TAG, "app '%s' never called App({...})", id);
        return false;
    }

    strlcpy(L.app_id, id, sizeof L.app_id);

    char *state = kb_store_read_state(id);
    st = state ? js_call_hook(L.eng, JS_HOOK_ON_RESUME, state, NULL)
               : js_call_hook(L.eng, JS_HOOK_ON_INIT, NULL, NULL);
    free(state);
    if (st != JS_OK) { app_fail("init/resume"); return false; }

    L.app_ok = true;
    return true;
}

/* Calls JS_HOOK_ON_RENDER once per stripe (docs/design/display-regions.md
 * §6) - once total on a non-striped board (caps.stripe_lines == 0),
 * identical to what this function did before stripes existed. jw.ui is
 * rebound to each stripe's gfx_ctx_t before every call, invisibly to app
 * code (still panel-absolute x/y - see js_ui.c). onRender() must be
 * idempotent under this model: it's the same call, run N times, not N
 * different calls - §6's whole argument for why that's an acceptable
 * (in fact already-satisfied, for this project's own examples)
 * constraint rather than a silent trap.
 *
 * The budget ceiling is tracked here, across the whole pass, not inside
 * js_call_hook() (which still gives every individual call its own
 * hook_budget_ms - a real backstop if one single stripe's call hangs
 * forever) - a hung app gets KB_RENDER_PASS_BUDGET_MS total, not that
 * much again per stripe. Kept in the launcher rather than the engine
 * layer on purpose: this is launcher/rendering-model logic, the engine
 * shouldn't need to know stripes exist at all (same reasoning as moving
 * the App()-only check out of js_load_app() and into app_boot()). */
static void app_render_if_dirty(void) {
    if (!L.app_ok) return;
    const board_desc_t *b = board_get();
    uint16_t stripe = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    int64_t pass_deadline_us = 0;
    bool any_dirty = false;

    if (b->display->begin_frame) b->display->begin_frame();
    for (int y = 0; y < b->caps.disp_h; y += stripe) {
        int h = stripe;
        if (y + h > b->caps.disp_h) h = b->caps.disp_h - y;
        gfx_ctx_t ctx = { .fb = L.fb, .board = b, .origin_y = y, .height = h };
        jw_ui_bind_fb(&ctx);

        if (pass_deadline_us == 0) {
            pass_deadline_us = esp_timer_get_time() + (int64_t)KB_RENDER_PASS_BUDGET_MS * 1000;
        } else if (esp_timer_get_time() >= pass_deadline_us) {
            ESP_LOGE(TAG, "app '%s' render pass exceeded %d ms across stripes, aborting frame",
                     L.app_id, KB_RENDER_PASS_BUDGET_MS);
            app_fail("render");
            return;
        }

        if (js_call_hook(L.eng, JS_HOOK_ON_RENDER, NULL, NULL) != JS_OK) {
            app_fail("render");
            return;
        }
        js_pump_jobs(L.eng);
        if (jw_ui_take_dirty()) {
            any_dirty = true;
            b->display->blit_region(0, y, b->caps.disp_w, h, L.fb);
        }
    }
    if (any_dirty) b->display->end_frame(false);
}

static void app_suspend_and_sleep(void) {
    if (L.app_ok) {
        char *state = NULL;
        if (js_call_hook(L.eng, JS_HOOK_ON_SUSPEND, NULL, &state) == JS_OK
            && state) {
            kb_store_write_state(L.app_id, state);
        }
        free(state);
    }
    js_destroy(L.eng);
    L.eng = NULL;
    kb_power_deep_sleep(); /* no return */
}

/* -------------------------------------------------------------- js_task */

/* src distinguishes the synthetic post-wake event (js_task's own call,
 * reflecting kb_power_wake_cause()) from real bus events (button ISR,
 * RTC timer, ...) - added to debug a suspected spurious-button-press
 * source (GPIO 0's dual role as PIN_BTN_UP and the auto-program strap,
 * see board.c's btn_isr comment). If a count only ever moves via
 * "bus-event" entries, it's real button presses (or noise on that
 * line, now debounced); if it moves via "synthetic-wake", something in
 * the wake-cause detection itself is wrong - a real bug hiding behind
 * what first looked like just noise. */
static void dispatch(const event_t *ev, const char *src) {
    ESP_LOGI(TAG, "dispatch: type=%d arg=%lu src=%s",
             ev->type, (unsigned long)ev->arg, src);
    char json[128];
    switch (ev->type) {
    case EV_BUTTON:
        kb_power_touch();
        snprintf(json, sizeof json, "{\"type\":\"button\",\"id\":%lu}",
                 (unsigned long)ev->arg);
        break;
    case EV_TICK_MINUTE:
        snprintf(json, sizeof json, "{\"type\":\"tick\"}");
        break;
    case EV_NET_RESULT:
        /* payload: net_result_t* -> serialize tag/status, hand body over.
         * TODO once jw.net exists. Free payload here in all cases. */
        free(ev->payload);
        return;
    case EV_IDLE_TIMEOUT:
        if (board_get()->caps.sleep_model_deep) app_suspend_and_sleep();
        return;
    default:
        return;
    }
    if (L.app_ok &&
        js_call_hook(L.eng, JS_HOOK_ON_EVENT, json, NULL) != JS_OK)
        app_fail("event");
}

static void js_task(void *arg) {
    (void)arg;
    const board_desc_t *b = board_get();

    /* Success criterion 2 (README): wake-to-render for the engine path,
     * counterpart to js_watchface_selftest()'s "C-path (no engine)" log -
     * same t0-to-render-ready shape, so the two numbers are comparable. */
    int64_t t0 = esp_timer_get_time();

    L.fb = heap_caps_malloc(board_fb_size(),
        b->caps.has_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
    /* Default/safe binding before app_boot() runs onInit/onResume (in
     * case either draws, unusual but not disallowed) - app_render_if_
     * dirty()'s own per-stripe loop rebinds for real once rendering
     * actually starts. height must match what L.fb was actually sized
     * for (board_fb_size(): one stripe, or the whole panel if
     * stripe_lines is 0) - not b->caps.disp_h directly, which would
     * overrun a stripe-sized buffer. */
    uint16_t boot_h = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    gfx_ctx_t boot_ctx = { .fb = L.fb, .board = b, .origin_y = 0, .height = boot_h };
    jw_ui_bind_fb(&boot_ctx);

    /* TODO: choose current app (NVS "last app", else first watchface). */
    char ids[4][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 4);
    if (n > 0) {
        app_boot(ids[0]);
    } else {
        /* Genuine empty state (project chat 2026-09-04: seed_apps.c, the
         * bring-up hack that always guaranteed at least one app, is gone
         * now that a real install path exists) - draw something instead
         * of leaving whatever was on the panel before, same no-engine "C
         * path" cadran_selftest()/net_svc's sync screen use. Not a
         * crash, not silent: kb_bus_receive()'s UINT32_MAX wait below
         * (app_ok stays false) is a real, intentional idle state, not a
         * missing-app bug. */
        ESP_LOGW(TAG, "no complications installed");
        /* Fixed content at panel-absolute coordinates, drawn fresh per
         * stripe - the same clip mechanism as everywhere else means
         * this needs no per-stripe logic of its own, just the loop. */
        uint16_t stripe = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
        if (b->display->begin_frame) b->display->begin_frame();
        for (int y = 0; y < b->caps.disp_h; y += stripe) {
            int h = stripe;
            if (y + h > b->caps.disp_h) h = b->caps.disp_h - y;
            gfx_ctx_t ctx = { .fb = L.fb, .board = b, .origin_y = y, .height = h };
            memset(L.fb, 0xFF, board_fb_size());
            gfx_draw_text(&ctx, 10, 80, "NO APPS", 2);
            gfx_draw_text(&ctx, 10, 110, "atelier push to install", 1);
            b->display->blit_region(0, y, b->caps.disp_w, h, L.fb);
        }
        b->display->end_frame(true);
    }

    /* synthesize wake event so the app can react to the wake cause */
    kb_wake_cause_t wc = kb_power_wake_cause();
    event_t wake = { .type = (wc == KB_WAKE_BUTTON) ? EV_BUTTON : EV_TICK_MINUTE };
    ESP_LOGI(TAG, "wake_cause=%d", wc);
    dispatch(&wake, "synthetic-wake");
    app_render_if_dirty();
    ESP_LOGI(TAG, "engine path: wake-to-render %lld us, free heap %u B",
             (long long)(esp_timer_get_time() - t0), (unsigned)esp_get_free_heap_size());

    event_t ev;
    for (;;) {
        uint32_t to = L.app_ok ? js_next_timer_ms(L.eng) : UINT32_MAX;
        if (kb_bus_receive(&ev, to)) {
            dispatch(&ev, "bus-event");
        } else if (L.app_ok) {
            js_dispatch_timers(L.eng);
        }
        app_render_if_dirty();
        if (!b->caps.sleep_model_deep) kb_power_idle();
    }
}

esp_err_t kb_launcher_start(void) {
    const board_desc_t *b = board_get();
    BaseType_t ok = xTaskCreatePinnedToCore(
        js_task, "kb_js", b->caps.js_task_stack / sizeof(StackType_t),
        NULL, 5, NULL, /* core */ portNUM_PROCESSORS > 1 ? 1 : 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
