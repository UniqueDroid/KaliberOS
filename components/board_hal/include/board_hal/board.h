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

/* Region-based (docs/design/display-regions.md): a board whose panel
 * doesn't fit a full framebuffer in RAM (caps.stripe_lines != 0, none
 * yet - Watchy sets 0, "one stripe covers the whole panel") renders in
 * horizontal bands. blit_region() takes a tightly-packed w*h buffer,
 * NOT an offset into a full-panel one - (x,y) are panel-absolute. */
typedef struct {
    esp_err_t (*init)(void);
    /* Optional (may be NULL) - most boards, including Watchy, don't
     * need per-frame setup and leave this out; callers must NULL-check
     * before calling it. A board that needs to open a write-window/
     * transaction spanning multiple blit_region() calls uses it. */
    esp_err_t (*begin_frame)(void);
    /* Blit one region: buf is w*h pixels in caps.disp_kind's native
     * format. Watchy always calls this once, with the whole panel
     * (stripe_lines=0) - identical to the old blit()'s body. */
    esp_err_t (*blit_region)(int x, int y, int w, int h, const uint8_t *buf);
    /* Push everything blitted since begin_frame() to glass. full=false
     * may use partial refresh (e-ink) or skip unchanged regions
     * (always-on RGB565, not implemented yet). Was update(bool full) -
     * same meaning, same ghosting-mitigation counter underneath on
     * Watchy, just renamed to pair with begin_frame(). */
    esp_err_t (*end_frame)(bool full);
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
    /* Optional (may be NULL) - true if USB VBUS is currently present.
     * Used to decide whether to enter net_svc's sync mode on wake, not
     * wired as a deep-sleep wake source itself (see watchy_v3/board.c's
     * comment on why: GPIO0/PIN_BTN_UP already burned this project once
     * on exactly that kind of assumption). */
    bool (*usb_connected)(void);
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
        /* 0 = one stripe covers the whole panel (every board today) -
         * see docs/design/display-regions.md. Nonzero: board_fb_size()
         * returns one stripe's size, not the full panel's; callers loop
         * ceil(disp_h / stripe_lines) times. */
        uint16_t         stripe_lines;
    } caps;
} board_desc_t;

/* Implemented exactly once by the selected board. */
const board_desc_t *board_get(void);

/* Size in bytes of the buffer a caller allocates and renders into - the
 * full panel if caps.stripe_lines is 0, one stripe's worth otherwise
 * (docs/design/display-regions.md §4). Every existing caller already
 * treats this as "the buffer I draw into," which stays true either way. */
size_t board_fb_size(void);

#ifdef __cplusplus
}
#endif
