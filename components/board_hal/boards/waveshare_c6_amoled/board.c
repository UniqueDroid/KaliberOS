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
 * §9's suggestion - 410*32*2 = 26,240 B/stripe) - this file covers the
 * first two. Waveshare's own BSP claims BSP_CAPS_BUTTONS=0, but that's
 * the BSP's own conservatism, not the PCB - two real buttons exist
 * (pins.h's PIN_BTN_BOOT/PIN_BTN_PWR, confirmed 2026-09-07 via a
 * separate hardware-verified project on the same exact board). Touch
 * (FT3168, tap+swipe as of 2026-09-07) and the two buttons are both
 * wired below; the AXP2101 PMIC (battery/USB-detect) is still a real
 * TODO, not silently assumed unnecessary.
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
#include "driver/i2c_master.h"
#include "driver/gpio.h"

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

/* FT3168 touch controller (FT5x06-family protocol, I2C addr 0x38,
 * pins.h) - the only input on this board (no physical buttons,
 * Waveshare's own BSP: BSP_CAPS_BUTTONS 0). Phase 1.1 scope (project
 * chat 2026-09-07, explicit constraint, extends the 2026-09-07
 * tap-only bring-up below): tap plus swipe-with-direction, nothing
 * finer - no multi-touch, no velocity/path data, no per-widget hit-
 * testing, no JS event API (that's a later wave, needs the lifecycle
 * contract js-api.md §6 doesn't settle). What crosses the HAL boundary
 * is a classified gesture (event_bus.h's EV_TOUCH_TAP /
 * EV_TOUCH_SWIPE, board_hal/board.h's kb_swipe_dir_t) - not register
 * layouts, interrupt behavior, or panel resolution. The FT3168-specific
 * bytes and the classification logic itself (§ touch_task below) never
 * leave this file.
 *
 * Register map + init sequence verified against Waveshare's own
 * Arduino_FT3x68.cpp/h (github.com/waveshareteam/ESP32-C6-Touch-AMOLED-
 * 2.06, examples/arduino/libraries/Arduino_DriveBus/src/touch_chip/,
 * fetched directly 2026-09-07, same "reference, not a dependency"
 * discipline as this board's display init sequence above) - not
 * guessed from the generic FT5x06 datasheet. Finger count (0x02) and
 * X1/Y1 position (0x03-0x06, high nibble + low byte per axis) read on
 * each interrupt.
 *
 * Two real findings from live bring-up (2026-09-07), both load-bearing:
 * (1) the reset pulse on PIN_TOUCH_RESET (GPIO10) is required, not
 * optional - without it the chip was silent on the *entire* I2C bus
 * (confirmed via a bus scan, not just "didn't ACK at 0x38"), matching a
 * chip sitting in hardware reset. pins.h's older comment worried this
 * pin might be shared with the LCD's reset line (GPIO11) - it isn't,
 * or if the silicon net is shared, pulsing it again after disp_init()
 * already completed causes no visible display issue (checked). (2)
 * power-mode register 0xA5 is set to 0x00 (active/continuous scan),
 * not Waveshare's own 0x01 (monitor/low-power) - monitor mode was
 * never retested after fixing the reset pulse, so it may well also
 * work now; active mode is simply what was confirmed working end to
 * end (real taps reaching the launcher, MENU and sync-mode both
 * reached) and this milestone's scope is "solve the bottleneck," not
 * "minimize touch power" - a real follow-up, not a silent choice. */
#define FT3168_I2C_ADDR        0x38
#define FT3168_REG_POWER_MODE  0xA5
#define FT3168_REG_FINGERNUM   0x02
#define FT3168_REG_X1_H        0x03
#define FT3168_REG_X1_L        0x04
#define FT3168_REG_Y1_H        0x05
#define FT3168_REG_Y1_L        0x06

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_touch_dev;
static TaskHandle_t s_touch_task;

static bool ft3168_read_reg(uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(s_touch_dev, &reg, 1, out, 1, 100) == ESP_OK;
}

static bool ft3168_read_xy(uint16_t *x, uint16_t *y) {
    uint8_t xh, xl, yh, yl;
    if (!ft3168_read_reg(FT3168_REG_X1_H, &xh) || !ft3168_read_reg(FT3168_REG_X1_L, &xl) ||
        !ft3168_read_reg(FT3168_REG_Y1_H, &yh) || !ft3168_read_reg(FT3168_REG_Y1_L, &yl)) {
        return false;
    }
    *x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    *y = (uint16_t)(((yh & 0x0F) << 8) | yl);
    return true;
}

/* Below this displacement, a touch is a tap: normal finger-down jitter,
 * not an intentional gesture. Above it, direction is whichever axis
 * moved further, sign gives up/down or left/right - a 4-way
 * classification, nothing finer (path curvature, velocity) per Phase
 * 1.1's own scope.
 *
 * Two components, not one (review round, project chat 2026-09-07): a
 * permille-of-panel-size threshold alone would mean the same *physical*
 * finger movement counts as a swipe on this board but not on a smaller/
 * denser one - a finger is the same size everywhere, the panel isn't.
 * PERMILLE scales with this panel's own dimensions (like the hit-target
 * rule in smartwatch-system/); ABSOLUTE_PX is a floor under it in real
 * pixels, sized from this specific panel's own known DPI (410×502 over
 * a 2.06" diagonal, ~314 DPI - Waveshare's own product name states the
 * diagonal) so a future, denser board can't derive an unrealistically
 * small threshold from the percentage alone. Whichever constant binds
 * on a given board is that board's own business - a lower-DPI board
 * might have ABSOLUTE_PX never actually be the larger of the two. */
#define SWIPE_MIN_DISPLACEMENT_PERMILLE 150
#define SWIPE_MIN_DISPLACEMENT_ABSOLUTE_PX 40
/* Sampled while a finger stays down, not purely interrupt-driven - the
 * FT3168's INT line pulses once per touch-down (Waveshare's own driver
 * comment on this board's own display init: "detected touch = one low
 * pulse"), not continuously while dragging, so there's nothing to wait
 * an interrupt for once a gesture starts; only when it ends (finger
 * lifted, FINGERNUM back to 0) or this safety cap is reached does the
 * gesture resolve. */
#define GESTURE_POLL_MS 20
#define GESTURE_MAX_MS  800

static void post_normalized(event_type_t type, uint16_t x, uint16_t y) {
    /* Normalized 0..1000 (permille of panel w/h) - event_bus.h's
     * EV_TOUCH_TAP contract, DISP_W/DISP_H stay inside this file. */
    uint32_t nx = (uint32_t)x * 1000 / DISP_W;
    uint32_t ny = (uint32_t)y * 1000 / DISP_H;
    if (nx > 1000) nx = 1000;
    if (ny > 1000) ny = 1000;
    event_t ev = { .type = type, .arg = (nx << 16) | ny };
    kb_bus_post(&ev);
}

/* I2C reads aren't ISR-safe (blocking, not IRAM-resident) - the ISR
 * below only wakes this task on the initial touch-down, which does the
 * actual register reads (both the one-shot tap case and, since Phase
 * 1.1, the poll-while-down loop that classifies a swipe) and posts the
 * bus event. Same split watchy_v3's btn_isr() doesn't need (a button
 * ISR already knows which button, nothing to read), the reason touch
 * can't just be "one more IRAM_ATTR handler". */
static void touch_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t fingers = 0;
        if (!ft3168_read_reg(FT3168_REG_FINGERNUM, &fingers) || fingers == 0) continue;

        uint16_t x0, y0;
        if (!ft3168_read_xy(&x0, &y0)) continue;
        uint16_t x_last = x0, y_last = y0;

        int64_t t_start = esp_timer_get_time();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(GESTURE_POLL_MS));
            if (!ft3168_read_reg(FT3168_REG_FINGERNUM, &fingers) || fingers == 0) break; /* lifted */
            if ((esp_timer_get_time() - t_start) > (int64_t)GESTURE_MAX_MS * 1000) break; /* safety cap */
            uint16_t x, y;
            if (!ft3168_read_xy(&x, &y)) break;
            x_last = x; y_last = y;
        }

        int32_t dx = (int32_t)x_last - (int32_t)x0;
        int32_t dy = (int32_t)y_last - (int32_t)y0;
        uint32_t adx = (uint32_t)(dx < 0 ? -dx : dx);
        uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy);
        uint16_t shorter = DISP_W < DISP_H ? DISP_W : DISP_H;
        uint32_t threshold = (uint32_t)shorter * SWIPE_MIN_DISPLACEMENT_PERMILLE / 1000;
        if (threshold < SWIPE_MIN_DISPLACEMENT_ABSOLUTE_PX) threshold = SWIPE_MIN_DISPLACEMENT_ABSOLUTE_PX;

        if (adx < threshold && ady < threshold) {
            post_normalized(EV_TOUCH_TAP, x0, y0);
        } else {
            kb_swipe_dir_t dir = (adx > ady)
                ? (dx > 0 ? KB_SWIPE_RIGHT : KB_SWIPE_LEFT)
                : (dy > 0 ? KB_SWIPE_DOWN  : KB_SWIPE_UP);
            event_t ev = { .type = EV_TOUCH_SWIPE, .arg = (uint32_t)dir };
            kb_bus_post(&ev);
        }
    }
}

static void IRAM_ATTR touch_isr(void *arg) {
    (void)arg;
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(s_touch_task, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* Two real physical buttons (pins.h's own comment on the source and the
 * polarity lesson behind it) - polled, not interrupt-driven, on purpose
 * (project chat 2026-09-07): a sibling project on the same exact board
 * hit real trouble with an interrupt-based guess on PIN_BTN_PWR
 * specifically, polling is the hardware-verified-working choice here,
 * not a Kaliber preference against interrupts in general (watchy_v3's
 * buttons stay interrupt-driven, that's correct there). Mapping is a
 * first default, not yet confirmed with Jan/Simon: BOOT -> KB_BTN_DOWN
 * (reaches sync mode from MENU, same as it always has - keeps that
 * reachable now that a full-screen tap no longer does), PWR ->
 * KB_BTN_BACK (a hardware fallback for leaving MENU, alongside the new
 * swipe gesture below - not the only way out, matching base-system.md
 * §3's "always get back out" promise with a second, independent path). */
static void button_poll_task(void *arg) {
    (void)arg;
    bool boot_was_pressed = false, pwr_was_pressed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

        bool boot_pressed = gpio_get_level(PIN_BTN_BOOT) == 0; /* active-low */
        if (boot_pressed && !boot_was_pressed) {
            event_t ev = { .type = EV_BUTTON, .arg = KB_BTN_DOWN };
            kb_bus_post(&ev);
        }
        boot_was_pressed = boot_pressed;

        bool pwr_pressed = gpio_get_level(PIN_BTN_PWR) == 1; /* active-high */
        if (pwr_pressed && !pwr_was_pressed) {
            event_t ev = { .type = EV_BUTTON, .arg = KB_BTN_BACK };
            kb_bus_post(&ev);
        }
        pwr_was_pressed = pwr_pressed;
    }
}

static esp_err_t input_init(void) {
    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << PIN_BTN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    const gpio_config_t pwr_cfg = {
        .pin_bit_mask = 1ULL << PIN_BTN_PWR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    xTaskCreate(button_poll_task, "buttons", 2560, NULL, 5, NULL);


    /* Reset pulse first, before any I2C traffic - see this file's
     * header comment above (finding 1): without it the chip never
     * responds at all. Sequence from Waveshare's own Arduino_FT3x68.cpp:
     * idle HIGH, pulse LOW 20ms, back HIGH, settle 50ms. */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_RESET,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(PIN_TOUCH_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_TOUCH_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TOUCH_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch: i2c bus init failed: %s", esp_err_to_name(err));
        return ESP_OK; /* Real gap, not fatal - board still boots/displays without touch. */
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT3168_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_touch_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch: i2c device add failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    /* Active/continuous scan mode, not Waveshare's own 0x01 (monitor)
     * - see this file's header comment (finding 2) for why. */
    uint8_t init_buf[2] = { FT3168_REG_POWER_MODE, 0x00 };
    err = i2c_master_transmit(s_touch_dev, init_buf, sizeof init_buf, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch: FT3168 not responding at 0x%02X (%s) - no touch input this boot",
                 FT3168_I2C_ADDR, esp_err_to_name(err));
        return ESP_OK;
    }

    xTaskCreate(touch_task, "touch", 3072, NULL, 5, &s_touch_task);

    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE, /* low pulse on touch, Waveshare's own driver comment */
    };
    gpio_config(&int_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_TOUCH_INT, touch_isr, NULL);

    ESP_LOGI(TAG, "FT3168 touch init done");
    return ESP_OK;
}

static esp_err_t input_arm_wake(void) {
    /* Always-on model (caps.sleep_model_deep = false, below) - no deep-
     * sleep wake sources to arm, same reasoning as power_wake_cause()'s
     * comment. */
    return ESP_OK;
}

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
