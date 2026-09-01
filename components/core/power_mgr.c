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
    ESP_LOGI(TAG, "entering deep sleep, tick in %lus",
             (unsigned long)s_cfg.tick_interval_s);
    b->power->sleep_prepare();
    esp_sleep_enable_timer_wakeup((uint64_t)s_cfg.tick_interval_s * 1000000ULL);
    esp_deep_sleep_start();
}

void kb_power_idle(void) {
    /* light model: TODO esp_pm config / brief light sleep between events */
}

kb_wake_cause_t kb_power_wake_cause(void) {
    return board_get()->power->wake_cause();
}
