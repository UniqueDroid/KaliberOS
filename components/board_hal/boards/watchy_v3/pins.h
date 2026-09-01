/**
 * Watchy v3 (ESP32-S3) pin map.
 *
 * Verified (01.09.2026) against PicoWatch's config.h, which runs on this
 * exact board in production (Arduino firmware, not ESP-IDF, but the same
 * physical pinout). Values match exactly - no discrepancies found.
 */
#pragma once

/* GDEH0154D67 200x200 e-ink on SPI */
#define PIN_DISP_SCK    47
#define PIN_DISP_MOSI   48
#define PIN_DISP_CS     33
#define PIN_DISP_DC     34
#define PIN_DISP_RES    35
#define PIN_DISP_BUSY   36

/* Buttons (active low, wake-capable RTC GPIOs where possible) */
#define PIN_BTN_MENU     7
#define PIN_BTN_BACK     6
#define PIN_BTN_UP       0
#define PIN_BTN_DOWN     8

#define PIN_VIB_MOTOR   17
#define PIN_BATT_ADC     9

#define DISP_W         200
#define DISP_H         200
