/**
 * Waveshare ESP32-C6-Touch-AMOLED-2.06 pinout.
 *
 * Source: the board's own official examples, not guessed from the
 * datasheet - examples/arduino/libraries/Mylibrary/pin_config.h and the
 * espressif-registry BSP (waveshare/esp32_c6_touch_amoled_2_06 v2.0.0,
 * include/bsp/esp32_c6_touch_amoled_2_06.h) both agree on every value
 * below (project chat 2026-09-05: fetched directly from
 * github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.06 and the ESP
 * Component Registry to cross-check, per Simon's "reference, not a
 * dependency" instruction - not embedded as a build dependency here,
 * only its pin values and init sequence were read).
 *
 * Panel: CO5300 controller (command-compatible with Espressif's
 * esp_lcd_sh8601 driver - the BSP builds against esp_lcd_sh8601, not a
 * CO5300-named component; no separate CO5300 driver exists, this is the
 * de facto ecosystem driver for it), 410x502 AMOLED, RGB565, QSPI.
 * Touch: FT3168 (FT5x06-family protocol), I2C.
 * Power: AXP2101 PMIC, I2C (same bus as touch) - not wired up yet, see
 * board.c's power_ops_t comment.
 */
#pragma once

/* Display - QSPI to the CO5300/SH8601 panel controller. GPIO0 doing
 * double duty as SCLK here (same as watchy_v3's PIN_BTN_UP) is this
 * board's own reference design, not a Kaliber choice - Waveshare's BSP
 * uses it as SCLK with no button on this pin at all (BSP_CAPS_BUTTONS
 * is 0 for this board), so the strap-pin noise history that pin has on
 * watchy_v3 doesn't apply the same way here; still worth remembering if
 * QSPI init ever misbehaves specifically around boot/reset. */
#define PIN_LCD_SCLK   0
#define PIN_LCD_DATA0  1   /* MOSI in single-SPI terms */
#define PIN_LCD_DATA1  2
#define PIN_LCD_DATA2  3
#define PIN_LCD_DATA3  4
#define PIN_LCD_CS     5
#define PIN_LCD_RESET  11

#define DISP_W 410
#define DISP_H 502

/* Touch - FT3168, I2C (same bus as the AXP2101 PMIC). Not wired up yet
 * (project chat 2026-09-05: "Buttons/Touch minimal, später") - kept here
 * because the pins are fixed hardware facts independent of when the
 * driver code lands. */
#define PIN_I2C_SDA    8
#define PIN_I2C_SCL    7
#define PIN_TOUCH_INT  15
#define PIN_TOUCH_RESET 10 /* shared with the LCD's reset line per the BSP - not a typo */
