/**
 * Waveshare ESP32-C6-Touch-AMOLED-2.06 - second board, docs/design/
 * display-regions.md's whole reason for existing (README: "stress test
 * for the HAL", the first board where stripe_lines is ever nonzero).
 *
 * Display driver: SH8601-family QSPI AMOLED controller (the panel is
 * marketed as "CO5300", but the ecosystem driver for it - Espressif's
 * component registry, ESP-IDF's own examples - is esp_lcd_sh8601; no
 * separate CO5300-named driver exists). Pin values and the init command
 * sequence are ported from Waveshare's own reference (pins.h's header
 * comment) - not guessed from the CO5300/SH8601 datasheet, same
 * discipline watchy_v3/board.c used for the SSD1681. Unlike watchy_v3,
 * this uses ESP-IDF's real esp_lcd_panel_* API (esp_lcd_new_panel_sh8601
 * + esp_lcd_panel_draw_bitmap()) rather than hand-rolled SPI transactions
 * - there's a maintained, correct driver for this exact controller
 * family in the ESP-IDF ecosystem, unlike for the SSD1681 e-ink
 * controller where none existed. Reference used per Simon's instruction
 * (project chat 2026-09-05): the official BSP (waveshare/
 * esp32_c6_touch_amoled_2_06) was fetched into a scratch project to read
 * its pins and init sequence, then discarded - it is not a build
 * dependency of this board.c. This board *does* depend directly on
 * waveshare/esp_lcd_sh8601 + espressif/esp_lcd_panel_io_additions
 * (components/board_hal/idf_component.yml) - that's the underlying
 * panel driver, not the BSP abstraction layer, the same category of
 * dependency as esp_driver_spi itself.
 *
 * Bring-up milestone (project chat 2026-09-05): boot + log, then display
 * with stripe_lines actually nonzero (=32, matching display-regions.md
 * §9's suggestion - 410*32*2 = 26,240 B/stripe), buttons/touch later -
 * this file covers the first two. No physical buttons on this board at
 * all (BSP_CAPS_BUTTONS=0 in Waveshare's own header) - touch (FT3168) and
 * the AXP2101 PMIC (battery/USB-detect) are both real TODOs below, not
 * silently assumed unnecessary.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board_hal/board.h"
#include "core/event_bus.h"
#include "pins.h"

static const char *TAG = "board.waveshare_c6_amoled";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

/* ---------------------------------------------------------------- display */

/* Ported byte-for-byte from the reference BSP's lcd_init_cmds[] (pins.h's
 * header comment) - column/row address ranges (0x2A/0x2B) already bake
 * in this exact panel's 0x16-pixel column offset and 410x502 active
 * area, so esp_lcd_panel_set_gap() below only needs to repeat the same
 * 0x16 for esp_lcd_panel_draw_bitmap()'s own coordinate math to agree
 * with what the controller was actually told. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static esp_err_t disp_init(void) {
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK, PIN_LCD_DATA0, PIN_LCD_DATA1, PIN_LCD_DATA2, PIN_LCD_DATA3,
        DISP_W * 32 * 2 /* one stripe's worth, see caps.stripe_lines below */);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0x16, 0)); /* matches s_init_cmds' 0x2A column offset */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "SH8601/CO5300 init done");
    return ESP_OK;
}

/* Region API (docs/design/display-regions.md): the first board where
 * this is ever called more than once per frame with a real nonzero y -
 * esp_lcd_panel_draw_bitmap()'s x_end/y_end are exclusive (standard
 * ESP-IDF esp_lcd convention), unlike watchy_v3's inclusive SSD1681
 * RAM-window registers. */
static esp_err_t disp_blit_region(int x, int y, int w, int h, const uint8_t *buf) {
    return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, buf);
}

/* AMOLED, not e-ink: no full-vs-partial refresh distinction to make here
 * (unlike watchy_v3's SSD1681 ghosting-mitigation counter) - every
 * blit_region() call already pushed real pixels straight to the panel.
 * `full` is accepted for interface symmetry, unused. */
static esp_err_t disp_end_frame(bool full) {
    (void)full;
    return ESP_OK;
}

static esp_err_t disp_sleep(void) {
    /* MIPI DCS sleep-in (0x10) - same command family s_init_cmds[0]'s
     * sleep-out (0x11) belongs to. Dead code today (sleep_model_deep is
     * false below, so kb_power_deep_sleep() - the only caller, via
     * power_sleep_prepare() - never runs for this board), kept correct
     * for whenever this board gets a light-sleep power model instead. */
    return esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0);
}

static const display_ops_t disp_ops = {
    .init = disp_init, .begin_frame = NULL, .blit_region = disp_blit_region,
    .end_frame = disp_end_frame, .sleep = disp_sleep,
};

/* ------------------------------------------------------------------ input */

/* No physical buttons on this board at all (Waveshare's own BSP header:
 * BSP_CAPS_BUTTONS 0) - touch (FT3168, pins.h) is the only input, and is
 * explicitly "later" for this bring-up milestone (project chat
 * 2026-09-05). A touch-driven launcher is a real design question of its
 * own (Simon's js-api.md roadmap note: "ein AMOLED mit Touch legt ein
 * anderes Wachmodell nahe als Timer-Ticks"), not something to bolt on
 * here as a KB_BTN_* mapping - there is nothing to map to. */
static esp_err_t input_init(void) { return ESP_OK; }
static esp_err_t input_arm_wake(void) { return ESP_OK; }

static const input_ops_t input_ops = {
    .init = input_init, .arm_wake = input_arm_wake,
};

/* ------------------------------------------------------------------ power */

/* AXP2101 PMIC (I2C, pins.h's PIN_I2C_SDA/SCL - shared with touch) owns
 * battery percentage and USB/charge detection on real hardware. Not
 * wired up yet (this milestone is display + boot only) - every function
 * below is a placeholder, not a measurement, flagged as such rather than
 * silently returning a plausible-looking fake number. */
static esp_err_t power_init(void) { return ESP_OK; }

static kb_wake_cause_t power_wake_cause(void) {
    /* Always-on model (caps.sleep_model_deep = false) - this board never
     * goes through esp_deep_sleep_start(), so ESP_SLEEP_WAKEUP_UNDEFINED
     * (a real power-on) is the only cause that's ever actually true
     * today. Kept as a real switch, not hardcoded, so a future light-
     * sleep model (touch/timer wake) has somewhere to plug in. */
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return KB_WAKE_RTC_TIMER;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return KB_WAKE_COLD;
    default: return KB_WAKE_OTHER;
    }
}

static esp_err_t power_sleep_prepare(void) {
    /* Dead code today, see disp_sleep()'s comment - sleep_model_deep is
     * false, kb_power_deep_sleep() is never called for this board. */
    return disp_sleep();
}

static uint32_t power_battery_mv(void) {
    /* TODO: AXP2101 fuel gauge over I2C - not implemented, this is a
     * placeholder, not a measurement (see module comment). */
    return 0;
}

static bool power_usb_connected(void) {
    /* TODO: AXP2101 VBUS status over I2C. Returning false unconditionally
     * means this board's sync mode (menu-triggered, same as watchy_v3
     * per 2026-09-05's change) never even offers to check USB - matches
     * "not wired up yet" honestly instead of guessing true. */
    return false;
}

/* .charging left unset (NULL): AXP2101 not wired up yet, same reason
 * .battery_mv/.usb_connected above are still placeholders - see
 * docs/design/js-api.md §4's Battery section for why this specific
 * capability is genuinely board-dependent (an I2C PMIC register read
 * here vs. a GPIO read on watchy_v3), not just unimplemented on both. */
static const power_ops_t power_ops = {
    .init = power_init, .wake_cause = power_wake_cause,
    .sleep_prepare = power_sleep_prepare, .battery_mv = power_battery_mv,
    .usb_connected = power_usb_connected,
};

/* --------------------------------------------------------------- sensors */

/* No IMU wired up (QMI8658 driver doesn't exist in this tree yet, a real
 * gap - see docs/design/js-api.md §4's Step section) - every field
 * NULL, same honesty as power_ops's missing .charging above. */
static const sensor_ops_t sensor_ops = { 0 };

/* No vibration motor on this board at all (not in pins.h - Waveshare's
 * own hardware has none). */
static const vibrator_ops_t vibrator_ops = { 0 };

/* ------------------------------------------------------------ descriptor */

static const board_desc_t desc = {
    .name     = "waveshare_c6_amoled",
    .display  = &disp_ops,
    .input    = &input_ops,
    .sensors  = &sensor_ops,
    .vibrator = &vibrator_ops,
    .power    = &power_ops,
    .caps = {
        /* ESP32-C6, no PSRAM (Waveshare's own sdkconfig.defaults for this
         * board configures no PSRAM options at all; matches display-
         * regions.md's original planning assumption of 512 KB SRAM, no
         * PSRAM). js_heap_budget is a starting guess, not yet measured -
         * this is exactly what the go/no-go gate (display-regions.md §9
         * step 3, main README) is for: log free heap after boot/engine-
         * init/WiFi-init/framebuffer-stripe-alloc on real hardware and
         * revisit this number from that, not the other way around. */
        .has_psram        = false,
        /* 64 KB (this file's first guess) made js_create() fail outright
         * on real hardware (project chat 2026-09-05: JS_NewRuntime2/
         * JS_NewContext returning NULL, no further detail logged at that
         * layer) - QuickJS's own baseline runtime overhead apparently
         * doesn't fit under 64 KB. 96 KB is watchy_v3's proven-working
         * value (same engine, same ABI, different chip) - matching it
         * here isn't a real measurement either, just a better-informed
         * starting point than a blind guess; the go/no-go gate itself
         * (display-regions.md §9 step 3) is still open. */
        .js_heap_budget   = 96 * 1024,
        .js_task_stack    = 32 * 1024,
        .engine           = KB_ENGINE_QUICKJS,
        .disp_w           = DISP_W,
        .disp_h           = DISP_H,
        .disp_kind        = DISP_AMOLED_RGB565,
        /* Always-on, not Watchy-style deep sleep - the whole point of
         * this board per the original task framing (docs/design/
         * display-regions.md's header). */
        .sleep_model_deep = false,
        /* Real stripe rendering for the first time (every other board
         * today uses 0) - display-regions.md §9's suggested starting
         * value, 410*32*2 = 26,240 B/stripe against ~512 KB total SRAM. */
        .stripe_lines     = 32,
    },
};

const board_desc_t *board_get(void) { return &desc; }

size_t board_fb_size(void) {
    uint16_t lines = desc.caps.stripe_lines ? desc.caps.stripe_lines : DISP_H;
    return (size_t)DISP_W * lines * 2; /* RGB565 = 2 B/px */
}
