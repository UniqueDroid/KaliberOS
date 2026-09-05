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
    /* Optional (may be NULL) - true if the battery is currently charging.
     * docs/design/js-api.md's Battery.isCharging(): genuinely board-
     * dependent by construction, not just unimplemented - a GPIO CHRG-pin
     * read and an AXP2101 I2C register read are two different
     * implementations of this same function pointer, not two states of
     * one. NULL here (both boards today) means jw.sensors.Battery()'s
     * isCharging() reports capability-unavailable (null), same as
     * battery_mv's own "no ADC wired up yet" gap - not a fake reading. */
    bool (*charging)(void);
} power_ops_t;

/* Pull-only, all optional (NULL = "this board can't answer this" -
 * same convention power_ops_t's usb_connected/charging already use, not
 * a second signaling mechanism). docs/design/js-api.md §4's Step:
 * board-specific IMU driver (BMA423 on watchy_v3, QMI8658 on
 * waveshare_c6_amoled) decides whether these are ever non-NULL: neither
 * board has one wired up yet (both real gaps, not placeholders -
 * board.c's own TODOs on each board say so), so both report
 * capability-unavailable today. A whole board without any sensors
 * at all (neither exists yet) still provides this struct with every
 * field NULL, same as power_ops_t already does for usb_connected. */
typedef struct {
    /* Current step count / today's step goal. Two separate function
     * pointers, not one struct return - either can independently not
     * exist on some future sensor (a pedometer with no configurable
     * goal, say) without forcing the other's capability along with it. */
    int32_t (*step_count)(void);
    int32_t (*step_target)(void);
} sensor_ops_t;

/* The one write/actuator capability in the first wave (docs/design/
 * js-api.md §2/§4) - deliberately minimal (start/stop only, no Zepp-
 * style scene/intensity selection, see that doc's Vibrator section for
 * why). The *safety* ceiling (max-duration auto-stop, stop-on-engine-
 * teardown) is NOT this struct's job - it lives in the native module
 * that calls these (unruh/modules/js_device.c), one layer up, so every
 * board's start() stays a plain "turn the motor on," not board-specific
 * timeout logic repeated per board. */
typedef struct {
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
} vibrator_ops_t;

/* ------------------------------------------------------------ description */

typedef struct {
    const char *name;

    const display_ops_t  *display;
    const input_ops_t    *input;
    const power_ops_t    *power;
    /* Never NULL (same as display/input/power) - a board with nothing
     * wired up yet still provides the struct, every field NULL, see
     * sensor_ops_t's/vibrator_ops_t's own comments. */
    const sensor_ops_t   *sensors;
    const vibrator_ops_t *vibrator;

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
