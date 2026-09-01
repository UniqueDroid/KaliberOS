/**
 * Kaliber core — central event bus.
 *
 * Single-consumer model: the js_task is the only consumer. Everything else
 * (ISRs, net_task, power manager) is a producer. This keeps the threading
 * model trivial and QuickJS-safe (the engine is not thread-safe).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_NONE = 0,
    EV_BUTTON,          /* arg = kb_button_t                                 */
    EV_TICK_MINUTE,
    EV_TIMER,           /* arg = js timer id                                 */
    EV_NET_RESULT,      /* payload = net_result_t*, ownership -> consumer    */
    EV_APP_INSTALLED,   /* payload = char* app id, ownership -> consumer     */
    EV_LOW_BATTERY,
    EV_IDLE_TIMEOUT,    /* power manager requests suspend                    */
    EV_SHUTDOWN,
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     arg;
    void        *payload;   /* heap-allocated; consumer frees                */
} event_t;

esp_err_t kb_bus_init(size_t queue_len);

/* Post from task context. Copies the event struct. */
esp_err_t kb_bus_post(const event_t *ev);

/* Post from ISR context. */
esp_err_t kb_bus_post_from_isr(const event_t *ev, bool *hp_task_woken);

/* Blocking receive with timeout; returns false on timeout.
 * Only ever called from js_task. */
bool kb_bus_receive(event_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
