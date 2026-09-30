/**
 * Kaliber core — power manager.
 *
 * Implements the two sleep strategies selected via caps.sleep_model_deep:
 *
 *  deep model  (Watchy): idle timeout -> EV_IDLE_TIMEOUT on the bus, the
 *      launcher suspends the app (state to NVS), then kb_power_deep_sleep()
 *      arms wake sources and enters deep sleep. Engine dies, world is
 *      rebuilt on wake.
 *
 *  light model (always-on boards): engine stays resident, kb_power_idle()
 *      is called between events and may enter light sleep / DFS. The idle
 *      timer runs here too (project chat 2026-09-30, base-system.md §3a's
 *      flagged open point, resolved) - not for power (nothing to save,
 *      the panel's on either way) but so a menu/app left open and
 *      forgotten still resolves back to the watchface on its own, the
 *      same "always get back out" promise the deep model already gets
 *      for free from its own sleep cycle. Uses a separate, deliberately
 *      more generous duration than the deep model's idle_timeout_ms
 *      (Simon's review: someone actively reading/scrolling a menu is a
 *      different situation than an app left running unattended, the
 *      deep model's tuned-for-that-case 15s would cut a real session
 *      short here, not just an abandoned one).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t idle_timeout_ms;      /* deep model: interactive session -> sleep */
    uint32_t light_idle_timeout_ms;/* light model: menu/app -> watchface       */
    uint32_t tick_interval_s;      /* RTC wake interval, 60 for watchfaces     */
} kb_power_cfg_t;

esp_err_t kb_power_init(const kb_power_cfg_t *cfg);

/* Reset the idle timer (call on user interaction: button, tap, swipe -
 * anything that means someone's actually there, not a tick/timer/net
 * event firing on its own). */
void kb_power_touch(void);

/* Stop the idle timer without rearming it - for a session that has its
 * own timeout already (net_svc's sync mode, 120s) and shouldn't also
 * race a second, independent one. Call kb_power_touch() again once that
 * session ends to resume normal idle tracking with a fresh window. */
void kb_power_pause(void);

/* Deep model only: never returns. */
void kb_power_deep_sleep(void);

/* Light model: hint that the bus is empty; may light-sleep briefly. */
void kb_power_idle(void);

kb_wake_cause_t kb_power_wake_cause(void);

#ifdef __cplusplus
}
#endif
