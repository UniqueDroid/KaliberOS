/**
 * Watchy v3 reference board.
 *
 * Display driver: SSD1681 controller, GDEH0154D67 200x200 1bpp panel.
 * Command sequence and timings ported from PicoWatch's Display.cpp
 * (GxEPD2-derived, real/working on this exact panel+controller) - not
 * guessed from the datasheet. Kept intentionally simpler than GxEPD2's
 * partial-rect windowing: our HAL only has a whole-framebuffer
 * blit()/update(bool full), no sub-rectangle API, so every blit sets the
 * full RAM window.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board_hal/board.h"
#include "core/event_bus.h"
#include "pins.h"

static const char *TAG = "board.watchy_v3";
static spi_device_handle_t s_spi;

/* ---------------------------------------------------------------- display */

/* BUSY reads HIGH while the panel is busy (PicoWatch's GxEPD2_EPD ctor
 * passes busy_level=HIGH for this exact panel). */
#define SSD1681_BUSY_TIMEOUT_MS 2000

static bool s_wrote_prev_buf; /* mirrors GxEPD2's "_initial_write" */

static void ssd1681_cmd(uint8_t cmd) {
    gpio_set_level(PIN_DISP_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    gpio_set_level(PIN_DISP_DC, 1);
}

static void ssd1681_data(const uint8_t *data, size_t len) {
    if (!len) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void ssd1681_data1(uint8_t byte) { ssd1681_data(&byte, 1); }

static void ssd1681_wait_busy(const char *why) {
    int waited = 0;
    while (gpio_get_level(PIN_DISP_BUSY) && waited < SSD1681_BUSY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (waited >= SSD1681_BUSY_TIMEOUT_MS)
        ESP_LOGW(TAG, "%s: BUSY timeout after %dms", why, waited);
}

static void ssd1681_reset(void) {
    gpio_set_level(PIN_DISP_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_DISP_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* Full RAM window (0,0,DISP_W,DISP_H) - see PicoWatch's
 * _setPartialRamArea(); we only ever use the whole-panel window. */
static void ssd1681_set_ram_window(void) {
    ssd1681_cmd(0x11); /* data entry mode */
    ssd1681_data1(0x03); /* x increase, y increase */
    ssd1681_cmd(0x44); /* set RAM X address range */
    ssd1681_data1(0);
    ssd1681_data1((DISP_W - 1) / 8);
    ssd1681_cmd(0x45); /* set RAM Y address range */
    ssd1681_data1(0);
    ssd1681_data1(0);
    ssd1681_data1((DISP_H - 1) % 256);
    ssd1681_data1((DISP_H - 1) / 256);
    ssd1681_cmd(0x4e); /* set RAM X address counter */
    ssd1681_data1(0);
    ssd1681_cmd(0x4f); /* set RAM Y address counter */
    ssd1681_data1(0);
    ssd1681_data1(0);
}

static esp_err_t disp_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_DISP_SCK,
        .mosi_io_num = PIN_DISP_MOSI,
        .miso_io_num = -1,
        /* Unused quad-SPI lines must be -1, not left at the struct's
         * zero-init default: 0 is a real GPIO (our PIN_BTN_UP!), so the
         * driver tried to claim it too - confirmed on real hardware
         * ("GPIO 0 is conflict with others and be overwritten"). */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISP_W * DISP_H / 8 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_DISP_CS,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DISP_DC) | (1ULL << PIN_DISP_RES),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    io.pin_bit_mask = 1ULL << PIN_DISP_BUSY;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    gpio_set_level(PIN_DISP_DC, 1);

    ssd1681_reset();

    ssd1681_cmd(0x01); /* driver output control: MUX = DISP_H-1 lines */
    ssd1681_data1((DISP_H - 1) & 0xff);
    ssd1681_data1(((DISP_H - 1) >> 8) & 0xff);
    ssd1681_data1(0x00);

    ssd1681_cmd(0x18); /* use built-in temperature sensor */
    ssd1681_data1(0x80);

    ssd1681_cmd(0x3c); /* border waveform: normal (non-dark) border */
    ssd1681_data1(0x05);

    ssd1681_set_ram_window();
    s_wrote_prev_buf = false;

    ESP_LOGI(TAG, "SSD1681 init done");
    return ESP_OK;
}

static esp_err_t disp_blit(const uint8_t *fb, size_t len) {
    ssd1681_set_ram_window();
    /* GxEPD2 also writes the "previous" buffer (0x26) once before the
     * first real update, so the differential update the controller does
     * internally doesn't ghost against undefined RAM content. Only
     * needed once - later blits only touch "current" (0x24). */
    if (!s_wrote_prev_buf) {
        ssd1681_cmd(0x26);
        ssd1681_data(fb, len);
        s_wrote_prev_buf = true;
    }
    ssd1681_cmd(0x24);
    ssd1681_data(fb, len);
    return ESP_OK;
}

/* Partial-LUT refreshes accumulate visible ghosting (the panel only
 * redraws changed pixels via a lighter waveform) - force a full refresh
 * every N partials to clear it. Board-level concern (SSD1681-specific),
 * not something a Complication should have to think about. */
#define SSD1681_FORCE_FULL_EVERY_N_PARTIALS 10

static esp_err_t disp_update(bool full) {
    /* Partial LUT assumes a full update already established a baseline
     * image (GxEPD2's _initial_refresh forces this the same way) - the
     * caller (launcher.c) doesn't track that, so enforce it here. */
    static bool s_did_first_update;
    static int  s_partials_since_full;
    if (!s_did_first_update) full = true;
    if (!full && ++s_partials_since_full >= SSD1681_FORCE_FULL_EVERY_N_PARTIALS) {
        full = true;
    }
    if (full) s_partials_since_full = 0;

    ssd1681_cmd(0x22); /* display update control 2 */
    ssd1681_data1(full ? 0xf4 : 0xfc); /* full vs partial LUT, see PicoWatch */
    ssd1681_cmd(0x20); /* activate display update sequence */
    ssd1681_wait_busy(full ? "update(full)" : "update(partial)");
    s_did_first_update = true;
    return ESP_OK;
}

static esp_err_t disp_sleep(void) {
    ssd1681_cmd(0x10); /* deep sleep mode */
    ssd1681_data1(0x01);
    return ESP_OK;
}

static const display_ops_t disp_ops = {
    .init = disp_init, .blit = disp_blit,
    .update = disp_update, .sleep = disp_sleep,
};

/* ------------------------------------------------------------------ input */

/* Timestamp debounce: a real press can't produce two edges closer than
 * this, so anything faster is bounce (mechanical) or noise (GPIO 0's
 * dual role as PIN_BTN_UP and the auto-program strap the USB-serial
 * chip's DTR/RTS drive - confirmed spurious-triggering during repeated
 * host-side reconnects, 01.09.2026). esp_timer_get_time() is ISR-safe. */
#define BTN_DEBOUNCE_US (30 * 1000)
static int64_t s_btn_last_us[KB_BTN_MAX];

static void IRAM_ATTR btn_isr(void *arg) {
    kb_button_t id = (kb_button_t)(uintptr_t)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_btn_last_us[id] < BTN_DEBOUNCE_US) return;
    s_btn_last_us[id] = now;

    event_t ev = { .type = EV_BUTTON, .arg = (uint32_t)(uintptr_t)arg };
    bool hpw = false;
    kb_bus_post_from_isr(&ev, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static esp_err_t input_init(void) {
    const struct { int pin; kb_button_t id; } map[] = {
        { PIN_BTN_UP, KB_BTN_UP },     { PIN_BTN_DOWN, KB_BTN_DOWN },
        { PIN_BTN_MENU, KB_BTN_SELECT }, { PIN_BTN_BACK, KB_BTN_BACK },
    };
    gpio_install_isr_service(0);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << map[i].pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* active-low, verified - see pins.h */
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        gpio_isr_handler_add(map[i].pin, btn_isr,
                             (void *)(uintptr_t)map[i].id);
    }
    return ESP_OK;
}

static esp_err_t input_arm_wake(void) {
    /* PIN_BTN_UP (GPIO0) deliberately excluded: it's also the USB auto-
     * program strap pin. Confirmed on real hardware (02.09.2026) that
     * this makes it fire the ext1 (level-triggered, no software debounce
     * possible) deep-sleep wake spuriously and repeatedly - the RTC
     * timer wake never got a chance to fire, kb_power_wake_cause()
     * reported KB_WAKE_BUTTON on every single cycle, and the synthetic
     * wake dispatch (launcher.c) kept incrementing hello's counter with
     * no button ever actually pressed. Runtime debounce in btn_isr()
     * doesn't help here - ext1 wake happens before the CPU (and its
     * software) is even running again. UP simply isn't usable as a
     * wake source on this board; still fine as a normal button once
     * awake (input_init()'s NEGEDGE handler debounces that fine). */
    uint64_t mask = (1ULL << PIN_BTN_DOWN) |
                    (1ULL << PIN_BTN_MENU) | (1ULL << PIN_BTN_BACK);
    return esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

static const input_ops_t input_ops = {
    .init = input_init, .arm_wake = input_arm_wake,
};

/* ------------------------------------------------------------------ power */

static esp_err_t power_init(void) {
    /* TODO: ADC oneshot unit for PIN_BATT_ADC, calibration */
    return ESP_OK;
}

static kb_wake_cause_t power_wake_cause(void) {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return KB_WAKE_RTC_TIMER;
    case ESP_SLEEP_WAKEUP_EXT1:  return KB_WAKE_BUTTON;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return KB_WAKE_COLD;
    default: return KB_WAKE_OTHER;
    }
}

static esp_err_t power_sleep_prepare(void) {
    disp_sleep();
    input_arm_wake();
    /* v3 uses the internal RTC; minute tick via timer wake is armed by the
     * power manager (esp_sleep_enable_timer_wakeup). */
    return ESP_OK;
}

static uint32_t power_battery_mv(void) {
    /* TODO: read + calibrate PIN_BATT_ADC, apply divider factor */
    return 0;
}

static const power_ops_t power_ops = {
    .init = power_init, .wake_cause = power_wake_cause,
    .sleep_prepare = power_sleep_prepare, .battery_mv = power_battery_mv,
};

/* ------------------------------------------------------------ descriptor */

static const board_desc_t desc = {
    .name    = "watchy_v3",
    .display = &disp_ops,
    .input   = &input_ops,
    .power   = &power_ops,
    .caps = {
        /* ESP32-S3FN8 (per watchy.sqfmi.com/docs/hardware's revision
         * table): 8 MB embedded flash, NO PSRAM. JS world lives in
         * internal SRAM - keep the budget honest and let apps fail with
         * a JS OOM instead of starving the system heap. Unverified
         * against the actual populated module though (see chat with
         * Jan, 01.09.2026) - confirm the exact part number before
         * trusting this over a schematic. */
        .has_psram        = false,
        .js_heap_budget   = 96 * 1024,
        .js_task_stack    = 32 * 1024,
        .engine           = KB_ENGINE_QUICKJS,
        .disp_w           = DISP_W,
        .disp_h           = DISP_H,
        .disp_kind        = DISP_EINK_1BIT,
        .sleep_model_deep = true,
    },
};

const board_desc_t *board_get(void) { return &desc; }

size_t board_fb_size(void) {
    return (size_t)DISP_W * DISP_H / 8;   /* 1 bpp */
}
