/**
 * Watchy v3 reference board.
 *
 * Display driver: minimal SSD1681-class driver on esp_driver_spi is the
 * plan (see README); the functions below are the bring-up skeleton with
 * the SPI plumbing sketched and the panel init sequence left as TODO.
 */
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_log.h"

#include "hal/board.h"
#include "core/event_bus.h"
#include "pins.h"

static const char *TAG = "board.watchy_v3";
static spi_device_handle_t s_spi;

/* ---------------------------------------------------------------- display */

static esp_err_t disp_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_DISP_SCK,
        .mosi_io_num = PIN_DISP_MOSI,
        .miso_io_num = -1,
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

    /* TODO: HW reset pulse + SSD1681 init sequence (SWRESET, driver output,
     * data entry mode, RAM window, temperature sensor, border). */
    ESP_LOGI(TAG, "display init (panel sequence TODO)");
    return ESP_OK;
}

static esp_err_t disp_blit(const uint8_t *fb, size_t len) {
    (void)fb; (void)len;
    /* TODO: 0x24 write RAM, DMA transfer of fb */
    return ESP_OK;
}

static esp_err_t disp_update(bool full) {
    (void)full;
    /* TODO: 0x22 display update control (full vs partial LUT), 0x20 activate,
     * wait BUSY */
    return ESP_OK;
}

static esp_err_t disp_sleep(void) {
    /* TODO: deep sleep mode 0x10, keep RAM if partial refresh planned */
    return ESP_OK;
}

static const display_ops_t disp_ops = {
    .init = disp_init, .blit = disp_blit,
    .update = disp_update, .sleep = disp_sleep,
};

/* ------------------------------------------------------------------ input */

static void IRAM_ATTR btn_isr(void *arg) {
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
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* verify polarity on v3! */
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        gpio_isr_handler_add(map[i].pin, btn_isr,
                             (void *)(uintptr_t)map[i].id);
    }
    /* TODO: debounce (esp_timer based, ~30 ms) */
    return ESP_OK;
}

static esp_err_t input_arm_wake(void) {
    /* ext1 wake on any button; verify RTC-capability of chosen GPIOs */
    uint64_t mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) |
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
        .has_psram        = true,
        .js_heap_budget   = 1024 * 1024,
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
