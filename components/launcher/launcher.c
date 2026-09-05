/**
 * Kaliber launcher — owns the js_task, the engine instance and the app
 * lifecycle. This is the only place that calls into Unruh.
 *
 * Wake flow (deep model):
 *   wake -> resolve launcher state (docs/design/launcher-states.md) ->
 *   engine up (WATCHFACE/APP) or native menu draw (MENU, no engine) ->
 *   dispatch wake event -> onRender -> blit -> idle loop ->
 *   EV_IDLE_TIMEOUT -> onSuspend -> persist state -> deep sleep.
 *
 * Three top-level states, persisted in NVS (core/launcher_state.h)
 * across deep sleep - WATCHFACE (base state, always exactly one active),
 * MENU (native C, no engine - must work even with an empty/unhealthy
 * store, same reasoning as the NO-APPS screen), APP (a booted
 * Complication, foreground). APP never survives a sleep as itself: the
 * idle timeout reverts it to WATCHFACE before persisting/sleeping (see
 * app_suspend_and_sleep()) - a plain minute tick doesn't interrupt an
 * app in use, but true inactivity does the same thing idle timeout
 * always did. MENU does survive a sleep, with one rule: a tick-wake
 * found sitting in MENU falls back to WATCHFACE before rendering
 * anything (an open menu is an easy-to-forget browsing state); a
 * button-wake stays in MENU, the user is actively there.
 *
 * The real menu (docs/design/launcher-states.md §3: scrollable list, own
 * roadmap step) doesn't exist yet - SELECT/BACK wire the full 3-state
 * cycle end to end against a placeholder screen instead, so the state
 * machine (the risky part: wake-cause interaction, NVS persistence,
 * engine lifecycle) is hardware-verified before the real UI (comparably
 * low-risk once this holds) gets built on top of it.
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
#include "core/launcher_state.h"
#include "unruh/engine.h"
#include "launcher/launcher.h"
#include "gfx/text.h"
#include "net_svc/net_svc.h"

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
    js_engine_t         *eng;
    kb_manifest_t        mf;
    uint8_t              *fb;
    char                  app_id[KB_APP_ID_MAX];
    bool                  app_ok;
    kb_launcher_state_t   state;
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
     * on v2/RAM-constrained boards - log it every boot, not just once.
     * Also the number that answers "does WATCHFACE -> MENU/APP actually
     * cost what it should" (project chat 2026-09-04) - compare against
     * teardown_engine_if_running()'s matching log on the way back. */
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

/* Tears down whatever engine is currently loaded (if any) - shared by
 * app_suspend_and_sleep() (idle timeout, about to sleep) and
 * enter_watchface() (explicit BACK from MENU/APP, does NOT sleep).
 * js_destroy(NULL) is safe (unruh/engine_quickjs.c) - a no-op call from
 * MENU state, where no engine was ever booted, costs nothing extra. */
static void teardown_engine_if_running(void) {
    if (L.app_ok) {
        char *state = NULL;
        if (js_call_hook(L.eng, JS_HOOK_ON_SUSPEND, NULL, &state) == JS_OK
            && state) {
            kb_store_write_state(L.app_id, state);
        }
        free(state);
    }
    if (L.eng) {
        js_destroy(L.eng);
        L.eng = NULL;
    }
    L.app_ok = false;
    ESP_LOGI(TAG, "engine torn down, free heap %u B", (unsigned)esp_get_free_heap_size());
}

/* ------------------------------------------------------- native screens */

/* Both MENU (§3: native C, must work even with an empty/unhealthy
 * store) and the empty-store fallback draw directly, no engine - same
 * "C path" cadran_selftest()/net_svc's sync screen already use. Kept as
 * their own small stripe loops rather than a shared helper, matching
 * how net_svc.c's draw_sync_screen() already does its own - the content
 * differs enough (and the loop is short enough) that sharing would cost
 * more in indirection than it saves. */
static void draw_no_apps_screen(void) {
    const board_desc_t *b = board_get();
    ESP_LOGW(TAG, "no complications installed");
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

/* Placeholder for docs/design/launcher-states.md §3's real scrollable
 * menu (own roadmap step, after this state machine is trusted) - shows
 * which state the watch is in, the only way to tell from the display
 * until the real menu exists (project chat 2026-09-04). SELECT enters
 * the single available Complication as APP; a real menu replaces this
 * wholesale, it doesn't grow into one. */
static void draw_menu_placeholder(void) {
    const board_desc_t *b = board_get();
    uint16_t stripe = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    if (b->display->begin_frame) b->display->begin_frame();
    for (int y = 0; y < b->caps.disp_h; y += stripe) {
        int h = stripe;
        if (y + h > b->caps.disp_h) h = b->caps.disp_h - y;
        gfx_ctx_t ctx = { .fb = L.fb, .board = b, .origin_y = y, .height = h };
        memset(L.fb, 0xFF, board_fb_size());
        gfx_draw_text(&ctx, 10, 80, "MENU", 2);
        gfx_draw_text(&ctx, 10, 110, "SELECT: open", 1);
        gfx_draw_text(&ctx, 10, 125, "BACK:   watchface", 1);
        gfx_draw_text(&ctx, 10, 140, "DOWN:   install", 1);
        b->display->blit_region(0, y, b->caps.disp_w, h, L.fb);
    }
    b->display->end_frame(true);
}

/* Bring-up self-check display (project chat 2026-09-05) - same category
 * and same eventual removal as the other *_selftest() functions in this
 * tree, but for the three wake-cause/state invariants this design doc
 * added (docs/design/launcher-states.md §1: tick-wake reverts MENU,
 * button-wake keeps MENU, idle-timeout reverts APP) that need a real
 * device and a human pressing buttons - a serial log can't be watched
 * live against a button press, but a screen can just be read. Shown as
 * its own full-screen flash, not overlaid on whatever renders next
 * (MENU/watchface/no-apps), so it doesn't touch app-drawn content or
 * need per-screen integration; same stripe-loop shape as
 * draw_no_apps_screen()/draw_menu_placeholder(), same reason those don't
 * share a helper either - the content and caller differ enough that
 * sharing would add indirection, not remove it. Blocking here for
 * KB_WAKE_CHECK_DISPLAY_MS is a one-time-per-check delay, not a steady-
 * state cost - acceptable for a bring-up aid, not something a real menu
 * would keep. */
#define KB_WAKE_CHECK_DISPLAY_MS 1500
static void draw_wake_check_screen(const char *msg) {
    const board_desc_t *b = board_get();
    ESP_LOGI(TAG, "wake-check: %s", msg);
    uint16_t stripe = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    if (b->display->begin_frame) b->display->begin_frame();
    for (int y = 0; y < b->caps.disp_h; y += stripe) {
        int h = stripe;
        if (y + h > b->caps.disp_h) h = b->caps.disp_h - y;
        gfx_ctx_t ctx = { .fb = L.fb, .board = b, .origin_y = y, .height = h };
        memset(L.fb, 0xFF, board_fb_size());
        gfx_draw_text(&ctx, 10, 80, "WAKE CHECK", 2);
        gfx_draw_text(&ctx, 10, 110, msg, 1);
        b->display->blit_region(0, y, b->caps.disp_w, h, L.fb);
    }
    b->display->end_frame(true);
    vTaskDelay(pdMS_TO_TICKS(KB_WAKE_CHECK_DISPLAY_MS));
}

/* ------------------------------------------------------- state helpers */

static bool find_first_watchface(char out_id[KB_APP_ID_MAX]) {
    char ids[4][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 4);
    for (int i = 0; i < n; i++) {
        kb_manifest_t mf;
        if (kb_store_read_manifest(ids[i], &mf) == ESP_OK && mf.type == KB_APP_WATCHFACE) {
            strlcpy(out_id, ids[i], KB_APP_ID_MAX);
            return true;
        }
    }
    return false;
}

/* Boots the active watchface (design doc §2): the persisted selection if
 * it still resolves, else the first type==watchface package (persisting
 * that as the new selection), else the NO-APPS screen. Does not itself
 * render - caller calls app_render_if_dirty() same as any other boot. */
static void render_current_face(void) {
    char id[KB_APP_ID_MAX];
    if (kb_launcher_active_face_get(id) == ESP_OK && app_boot(id)) return;
    if (find_first_watchface(id) && app_boot(id)) {
        kb_launcher_active_face_set(id);
        return;
    }
    draw_no_apps_screen();
}

static void enter_watchface(void) {
    teardown_engine_if_running();
    L.state = KB_LSTATE_WATCHFACE;
    kb_launcher_state_set(L.state);
    render_current_face();
}

static void enter_menu(void) {
    teardown_engine_if_running();
    L.state = KB_LSTATE_MENU;
    kb_launcher_state_set(L.state);
    draw_menu_placeholder();
}

/* Placeholder for real menu navigation (§3) - "the single available
 * Complication" stands in for "whichever the user picked". */
static void enter_app_placeholder(void) {
    char ids[4][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 4);
    if (n == 0) { draw_no_apps_screen(); return; }
    L.state = KB_LSTATE_APP;
    kb_launcher_state_set(L.state);
    if (!app_boot(ids[0])) draw_no_apps_screen();
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
        /* State-label overlay (project chat 2026-09-04): the only way to
         * read "which state is the watch in" off the display until the
         * real menu (§3) exists - drawn into the same frame as the
         * app's own content, after its onRender() but before the blit,
         * so it costs no extra refresh. WATCHFACE state doesn't get one
         * - once a real watchface is installed this is meant to look
         * like a normal watchface, not a debug build. */
        if (L.state == KB_LSTATE_APP) {
            char label[80]; /* generous: GCC's format-truncation check assumes
                              * L.app_id (declared KB_APP_ID_MAX=64) could be
                              * fully used, "APP: " + 64 + NUL needs more than
                              * a tighter buffer here. */
            snprintf(label, sizeof label, "APP: %s", L.app_id);
            gfx_draw_text(&ctx, 10, 190, label, 1);
        }
        if (jw_ui_take_dirty()) {
            any_dirty = true;
            b->display->blit_region(0, y, b->caps.disp_w, h, L.fb);
        }
    }
    if (any_dirty) b->display->end_frame(false);
}

static void app_suspend_and_sleep(void) {
    /* APP never survives a sleep as itself (design doc §1) - idle
     * timeout reverts it to WATCHFACE before persisting, same as a
     * user-initiated BACK would, just triggered by inactivity instead
     * of a button. MENU does survive (see the wake-cause rule in
     * js_task()), persisted as-is. */
    bool was_app = (L.state == KB_LSTATE_APP); /* for the wake-check screen below */
    if (was_app) L.state = KB_LSTATE_WATCHFACE;
    kb_launcher_state_set(L.state);
    if (was_app) draw_wake_check_screen("IDLE: APP->FACE OK");
    teardown_engine_if_running();
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
    ESP_LOGI(TAG, "dispatch: type=%d arg=%lu src=%s state=%d",
             ev->type, (unsigned long)ev->arg, src, L.state);
    char json[128];
    switch (ev->type) {
    case EV_BUTTON:
        kb_power_touch();
        /* State transitions (docs/design/launcher-states.md §1) -
         * SELECT/BACK are consumed here, never forwarded to an app's
         * own onEvent when they trigger one. Any other button (or
         * SELECT/BACK when they *don't* apply - e.g. BACK in WATCHFACE,
         * a no-op per §1) falls through to the normal forward-to-app
         * path below, unchanged from before this state machine existed. */
        if (ev->arg == KB_BTN_SELECT && L.state == KB_LSTATE_WATCHFACE) {
            enter_menu();
            return;
        }
        if (ev->arg == KB_BTN_SELECT && L.state == KB_LSTATE_MENU) {
            enter_app_placeholder();
            return;
        }
        if (ev->arg == KB_BTN_BACK && L.state != KB_LSTATE_WATCHFACE) {
            enter_watchface();
            return;
        }
        /* Sync mode, on demand (project chat 2026-09-05) - replaces the
         * old "every wake with USB attached" auto-trigger (main.c used
         * to do this; retired, see that file's comment). Blocking here
         * is fine, same as it was blocking in main.c before js_task even
         * started: kb_net_svc_run_sync_mode() only returns once its own
         * timeout or an install happens, and MENU has nothing else to do
         * meanwhile. DOWN, not UP - UP is PIN_BTN_UP/GPIO0, the pin with
         * its own documented USB-noise history (board.c), not a button
         * worth pairing with "now go turn WiFi on". */
        if (ev->arg == KB_BTN_DOWN && L.state == KB_LSTATE_MENU) {
            kb_net_svc_run_sync_mode();
            draw_menu_placeholder();
            return;
        }
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
    /* No-op in MENU state (L.app_ok is always false there - no engine
     * ever boots for the native menu) and for a WATCHFACE-state BACK
     * (§1: "BACK: no-op"), both already covered by app_ok being false
     * or this simply not being reached. */
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

    /* Launcher state (docs/design/launcher-states.md §1): resolved once
     * per wake, before anything renders. The only rule applied here -
     * everything else is a live-session transition, handled in
     * dispatch() above. APP is never actually the persisted value in
     * practice (app_suspend_and_sleep() always reverts it first) but
     * degrade safely if it somehow were, same as an unrecognized value
     * (kb_launcher_state_get() already clamps that case). */
    L.state = kb_launcher_state_get();
    kb_wake_cause_t wc = kb_power_wake_cause();
    kb_launcher_state_t persisted = L.state; /* pre-revert, for the wake-check screen below */
    if (L.state == KB_LSTATE_MENU && wc == KB_WAKE_RTC_TIMER) L.state = KB_LSTATE_WATCHFACE;
    if (L.state == KB_LSTATE_APP) L.state = KB_LSTATE_WATCHFACE;

    /* Wake-check screen (see draw_wake_check_screen()'s comment) - only
     * the two invariants a MENU wake actually exercises; a WATCHFACE or
     * APP wake has nothing to confirm here (APP's own check is in
     * app_suspend_and_sleep(), the only place it's ever exited from). */
    if (persisted == KB_LSTATE_MENU) {
        if (wc == KB_WAKE_RTC_TIMER) draw_wake_check_screen("TICK: MENU->FACE OK");
        else if (wc == KB_WAKE_BUTTON) draw_wake_check_screen("BTN-WAKE: MENU OK");
    }

    if (L.state == KB_LSTATE_MENU) {
        draw_menu_placeholder();
    } else {
        render_current_face();
    }

    /* synthesize wake event so the app can react to the wake cause */
    event_t wake = { .type = (wc == KB_WAKE_BUTTON) ? EV_BUTTON : EV_TICK_MINUTE };
    ESP_LOGI(TAG, "wake_cause=%d state=%d", wc, L.state);
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
