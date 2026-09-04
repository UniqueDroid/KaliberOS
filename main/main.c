/**
 * Kaliber entry point. Order matters:
 * board -> bus -> store -> power -> launcher (js_task takes over).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "board_hal/board.h"
#include "core/event_bus.h"
#include "core/power_mgr.h"
#include "core/app_store.h"
#include "launcher/launcher.h"
#include "cadran/cadran.h"
#include "seed_apps.h"

static const char *TAG = "kaliber";

extern void js_watchface_selftest(void);
extern void kb_store_install_selftest(void);

/* js_watchface_selftest() creates a real QuickJS engine (JS_SetMaxStackSize
 * up to caps.js_task_stack - 12k, e.g. ~20k on watchy_v3) - too big for
 * main_task's own stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE, 8k here). Unlike
 * cadran_selftest() (pure C, no engine, fine on any stack), this needs a
 * task sized like the real js_task or QuickJS's internal guard would think
 * it has headroom main_task doesn't actually have - a real overflow, not a
 * style nitpick. Run it on its own task, sized to match, and block until
 * done so it can't race the launcher's own engine for heap. */
static SemaphoreHandle_t s_wf_selftest_done;

static void watchface_selftest_task(void *arg) {
    (void)arg;
    js_watchface_selftest();
    xSemaphoreGive(s_wf_selftest_done);
    vTaskDelete(NULL);
}

void app_main(void) {
    const board_desc_t *b = board_get();
    ESP_LOGI(TAG, "Kaliber on %s (engine=%d, psram=%d, deep=%d)",
             b->name, b->caps.engine, b->caps.has_psram,
             b->caps.sleep_model_deep);

    ESP_ERROR_CHECK(b->power->init());
    ESP_ERROR_CHECK(b->display->init());

    /* Bring-up only: proves the Cadran loader/renderer/provider round-trip
     * on real hardware. Remove once cadran_render() is wired into the
     * launcher for real (roadmap step 6) - see cadran.h. Must run after
     * display->init(): it optionally blits its test framebuffer to the
     * real panel (see selftest.c), and its own log line is otherwise the
     * second line of app_main() - too early for the host to have the USB
     * connection up, so it never showed in any serial capture regardless
     * of how tight the reconnect loop on the host side was. */
    cadran_selftest();

    /* Bring-up only, same rationale, see watchface_selftest_task() above -
     * proves the WatchFace()/serializer/loader/renderer round trip. Remove
     * alongside cadran_selftest() at roadmap step 6. */
    s_wf_selftest_done = xSemaphoreCreateBinary();
    xTaskCreate(watchface_selftest_task, "wf_selftest", b->caps.js_task_stack, NULL, 5, NULL);
    xSemaphoreTake(s_wf_selftest_done, portMAX_DELAY);
    vSemaphoreDelete(s_wf_selftest_done);

    ESP_ERROR_CHECK(kb_bus_init(16));
    ESP_ERROR_CHECK(b->input->init());

    /* kb_store_install()'s HMAC key lives in NVS (per-device, generated on
     * first boot - see app_store.c), needs the NVS partition initialized
     * before anything touches it. Standard ESP-IDF idiom: a stale/wrong-
     * version NVS partition (e.g. after a layout change) fails once with
     * a specific error, erase-and-retry is the documented recovery, not a
     * silent data-loss risk - there's nothing durable in NVS yet at this
     * project stage besides that key. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(kb_store_init());

    /* Bring-up only, same rationale as cadran_selftest()/
     * js_watchface_selftest() above. Ran behind a one-shot NVS guard for
     * several days (project chat 2026-09-03/04) while a real hang in
     * kb_store_install() was traced down to a 10 KB stack array in this
     * selftest's own tamper test (see app_store.c) - fixed there, safe
     * to call unconditionally now. Remove alongside the other bring-up
     * selftests once net_svc.c exercises this path for real. */
    kb_store_install_selftest();

    /* Bring-up only: see seed_apps.c. Only one at a time - the launcher
     * boots whichever app kb_store_list() returns first (readdir order),
     * so seeding two here would make the boot target unpredictable.
     * seed_budget_hog_app() was used to test success criterion 3 (budget
     * handler, confirmed working 01.09.2026) - swap back if that needs
     * retesting, e.g. after touching engine_quickjs.c's interrupt path. */
    seed_hello_app();

    kb_power_cfg_t pcfg = { .idle_timeout_ms = 15000, .tick_interval_s = 60 };
    ESP_ERROR_CHECK(kb_power_init(&pcfg));

    ESP_ERROR_CHECK(kb_launcher_start());
    /* app_main returns; FreeRTOS keeps running our tasks. */
}
