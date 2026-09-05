#include <time.h>
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "core/power_mgr.h"
#include "core/event_bus.h"

static const char *TAG = "power";
static kb_power_cfg_t s_cfg;
static esp_timer_handle_t s_idle;

static void idle_cb(void *arg) {
    (void)arg;
    event_t ev = { .type = EV_IDLE_TIMEOUT };
    kb_bus_post(&ev);
}

esp_err_t kb_power_init(const kb_power_cfg_t *cfg) {
    s_cfg = *cfg;
    if (board_get()->caps.sleep_model_deep) {
        const esp_timer_create_args_t a = { .callback = idle_cb, .name = "kb_idle" };
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_idle));
        kb_power_touch();
    }
    return ESP_OK;
}

void kb_power_touch(void) {
    if (!s_idle) return;
    esp_timer_stop(s_idle);
    esp_timer_start_once(s_idle, (uint64_t)s_cfg.idle_timeout_ms * 1000);
}

void kb_power_deep_sleep(void) {
    const board_desc_t *b = board_get();

    /* Align to the next wall-clock minute boundary, not a flat
     * tick_interval_s from whatever instant sleep_prepare() happens to
     * run at (project chat 2026-09-05, found live: the clock face showed
     * the previous minute for up to 59s after every tick-wake, since
     * "60s from now" almost never lands exactly on a :00 second). Only
     * meaningful for the once-a-minute tick specifically (tick_interval_s
     * == 60 - anything else configured has no such natural boundary to
     * align to) and only once the clock is actually set - same tm_year
     * >= 100 convention cadran/providers.c uses to tell "never synced"
     * from "really is year 1970"; with no time source yet there's no
     * boundary to align to either, so fall back to the flat interval
     * exactly as before. */
    uint64_t sleep_us = (uint64_t)s_cfg.tick_interval_s * 1000000ULL;
    if (s_cfg.tick_interval_s == 60) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        if (tmv.tm_year >= 100) {
            int secs_to_next_minute = 60 - tmv.tm_sec;
            if (secs_to_next_minute <= 0) secs_to_next_minute = 60;
            sleep_us = (uint64_t)secs_to_next_minute * 1000000ULL;
        }
    }
    ESP_LOGI(TAG, "entering deep sleep, tick in %llu us",
             (unsigned long long)sleep_us);
    b->power->sleep_prepare();
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}

void kb_power_idle(void) {
    /* light model: TODO esp_pm config / brief light sleep between events */
}

kb_wake_cause_t kb_power_wake_cause(void) {
    return board_get()->power->wake_cause();
}
