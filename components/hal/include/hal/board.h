/**
 * Kaliber HAL — board abstraction.
 *
 * A board is selected at compile time (CONFIG_KALIBER_BOARD_*) and provides
 * exactly one board_desc_t. Core and JS layers query capabilities, they never
 * know board names. No #ifdef BOARD_X outside of components/hal/boards/.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

typedef enum {
    DISP_EINK_1BIT,      /* canonical fb: 1 bpp, MSB first, row-major        */
    DISP_AMOLED_RGB565,  /* canonical fb: 16 bpp RGB565                      */
} disp_kind_t;

typedef enum {
    KB_ENGINE_QUICKJS,
    KB_ENGINE_MQUICKJS,
} js_engine_kind_t;

typedef enum {
    KB_WAKE_COLD,        /* power-on / reset                                 */
    KB_WAKE_RTC_TIMER,   /* minute tick etc.                                 */
    KB_WAKE_BUTTON,
    KB_WAKE_OTHER,
} kb_wake_cause_t;

typedef enum {
    KB_BTN_UP, KB_BTN_DOWN, KB_BTN_SELECT, KB_BTN_BACK,
    KB_BTN_MAX,
} kb_button_t;

/* -------------------------------------------------------------------- ops */

typedef struct {
    esp_err_t (*init)(void);
    /* Blit the full canonical framebuffer owned by the caller. */
    esp_err_t (*blit)(const uint8_t *fb, size_t len);
    /* Push blitted content to glass. full=false may use partial refresh. */
    esp_err_t (*update)(bool full);
    /* Put panel into deepest sleep state (before deep sleep / idle). */
    esp_err_t (*sleep)(void);
} display_ops_t;

typedef struct {
    /* Install ISRs. Events are posted to the core event bus (event_bus.h),
     * the HAL never calls into JS. */
    esp_err_t (*init)(void);
    /* Configure wake sources so buttons work out of deep sleep. */
    esp_err_t (*arm_wake)(void);
} input_ops_t;

typedef struct {
    esp_err_t (*init)(void);
    kb_wake_cause_t (*wake_cause)(void);
    /* Board-specific teardown immediately before esp_deep_sleep_start(). */
    esp_err_t (*sleep_prepare)(void);
    uint32_t (*battery_mv)(void);
} power_ops_t;

/* ------------------------------------------------------------ description */

typedef struct {
    const char *name;

    const display_ops_t *display;
    const input_ops_t   *input;
    const power_ops_t   *power;

    struct {
        bool             has_psram;
        size_t           js_heap_budget;   /* bytes granted to the JS world  */
        size_t           js_task_stack;    /* bytes                          */
        js_engine_kind_t engine;
        uint16_t         disp_w, disp_h;
        disp_kind_t      disp_kind;
        bool             sleep_model_deep; /* true: Watchy-style deep sleep  */
    } caps;
} board_desc_t;

/* Implemented exactly once by the selected board. */
const board_desc_t *board_get(void);

/* Size in bytes of the canonical framebuffer for this board. */
size_t board_fb_size(void);

#ifdef __cplusplus
}
#endif
