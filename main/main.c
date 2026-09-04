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
#include "net_svc/net_svc.h"

static const char *TAG = "kaliber";

extern void js_watchface_selftest(void);
extern unsigned kb_store_install_selftest(void);

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
     * selftests once net_svc.c has exercised the real path a few times
     * on hardware (below, gated on USB - not yet on every boot).
     *
     * Verdict captured and re-logged after the sync-mode block below, not
     * just here - this runs at ~1s into boot, squarely inside the ~15s
     * post-USB-reset window this project's serial capture setup has never
     * reliably caught (project chat, all session); re-emitting it once
     * WiFi has had time to come up gives it a second, much later chance
     * to land in whatever capture is actually running. */
    unsigned store_selftest_bits = kb_store_install_selftest();

    kb_power_cfg_t pcfg = { .idle_timeout_ms = 15000, .tick_interval_s = 60 };
    ESP_ERROR_CHECK(kb_power_init(&pcfg));

    /* Sync mode trigger (project chat 2026-09-04): checked once per wake,
     * not wired as a deep-sleep wake source - that's exactly the kind of
     * change that already cost this project a real bug once (GPIO0/
     * PIN_BTN_UP's spurious ext1 wake, see board.c's input_arm_wake()).
     * While plugged in, every wake opens one sync-mode window before the
     * normal render path runs; unplugging (or timing out) falls straight
     * through to kb_launcher_start() below either way, so a watch left
     * plugged in overnight doesn't get stuck - it just spends each wake's
     * first KALIBER_NET_SYNC_TIMEOUT_S seconds with the endpoint open. */
    if (b->power->usb_connected && b->power->usb_connected()) {
        ESP_LOGI(TAG, "USB connected - entering sync mode before normal render");
        kb_net_svc_run_sync_mode();
    }

    /* Deliberately after the sync-mode block, not right where the selftest
     * itself ran (see the comment above kb_store_install_selftest()'s call
     * site) - by now enough real time has passed that this line reliably
     * lands in whatever host-side capture is running. */
    /* Bitmask, not just pass/fail (project chat 2026-09-04: a bare bool
     * here cost a whole extra flash/test cycle to find out WHICH check
     * failed, since app_store.c's own detailed line runs too early to
     * reliably capture) - bit 0 install, 1 listed, 2 manifest, 3
     * bytecode, 4 tamper-rejected; see app_store.c's KB_STORE_SELFTEST_*. */
    ESP_LOGI(TAG, "store selftest: %s (bits=0x%02x: install=%d listed=%d manifest=%d bytecode=%d tamper=%d)",
             store_selftest_bits == 0x1F ? "PASS" : "FAIL", store_selftest_bits,
             (store_selftest_bits >> 0) & 1, (store_selftest_bits >> 1) & 1,
             (store_selftest_bits >> 2) & 1, (store_selftest_bits >> 3) & 1,
             (store_selftest_bits >> 4) & 1);

    ESP_ERROR_CHECK(kb_launcher_start());
    /* app_main returns; FreeRTOS keeps running our tasks. */
}
