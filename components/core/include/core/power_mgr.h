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
 *      is called between events and may enter light sleep / DFS.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t idle_timeout_ms;      /* interactive session -> sleep          */
    uint32_t tick_interval_s;      /* RTC wake interval, 60 for watchfaces  */
} kb_power_cfg_t;

esp_err_t kb_power_init(const kb_power_cfg_t *cfg);

/* Reset the idle timer (call on user interaction). */
void kb_power_touch(void);

/* Deep model only: never returns. */
void kb_power_deep_sleep(void);

/* Light model: hint that the bus is empty; may light-sleep briefly. */
void kb_power_idle(void);

kb_wake_cause_t kb_power_wake_cause(void);

#ifdef __cplusplus
}
#endif
