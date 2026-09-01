/**
 * Kaliber entry point. Order matters:
 * board -> bus -> store -> power -> launcher (js_task takes over).
 */
#include "esp_log.h"
#include "board_hal/board.h"
#include "core/event_bus.h"
#include "core/power_mgr.h"
#include "core/app_store.h"
#include "launcher/launcher.h"
#include "cadran/cadran.h"
#include "seed_hello.h"

static const char *TAG = "kaliber";

void app_main(void) {
    const board_desc_t *b = board_get();
    ESP_LOGI(TAG, "Kaliber on %s (engine=%d, psram=%d, deep=%d)",
             b->name, b->caps.engine, b->caps.has_psram,
             b->caps.sleep_model_deep);

    /* Bring-up only: proves the Cadran loader/renderer/provider round-trip
     * on real hardware. Remove once cadran_render() is wired into the
     * launcher for real (roadmap step 6) - see cadran.h. */
    cadran_selftest();

    ESP_ERROR_CHECK(b->power->init());
    ESP_ERROR_CHECK(b->display->init());
    ESP_ERROR_CHECK(kb_bus_init(16));
    ESP_ERROR_CHECK(b->input->init());
    ESP_ERROR_CHECK(kb_store_init());
    /* Bring-up only: see seed_hello.c. */
    seed_hello_app();

    kb_power_cfg_t pcfg = { .idle_timeout_ms = 15000, .tick_interval_s = 60 };
    ESP_ERROR_CHECK(kb_power_init(&pcfg));

    ESP_ERROR_CHECK(kb_launcher_start());
    /* app_main returns; FreeRTOS keeps running our tasks. */
}
