/**
 * Watchy v3 (ESP32-S3) pin map.
 *
 * Verified (01.09.2026) against PicoWatch's config.h/PicoWatch.cpp, which
 * runs on this exact board in production (Arduino firmware, not ESP-IDF,
 * but the same physical pinout). Values match exactly. Button polarity
 * also cross-checked against the real deep-sleep wake code
 * (ARDUINO_ESP32S3_DEV branch): internal pull-up + ext1 ANY_LOW, i.e.
 * active-LOW - a later "fix" package claiming active-HIGH/external
 * pull-downs for v3 was checked against this same source and is wrong
 * for v3 (that's the V2/ESP32-PICO-D4 branch's polarity, not v3's).
 */
#pragma once

/* GDEH0154D67 200x200 e-ink on SPI */
#define PIN_DISP_SCK    47
#define PIN_DISP_MOSI   48
#define PIN_DISP_MISO   46   /* unused by the panel, bus definition only */
#define PIN_DISP_CS     33
#define PIN_DISP_DC     34
#define PIN_DISP_RES    35
#define PIN_DISP_BUSY   36

/* Buttons: active-LOW, internal pull-up, ext1 wake ANY_LOW - see board.c */
#define PIN_BTN_MENU     7
#define PIN_BTN_BACK     6
#define PIN_BTN_UP       0
#define PIN_BTN_DOWN     8

/* I2C bus: BMA423 accelerometer (not wired up in board.c yet) */
#define PIN_I2C_SDA     12
#define PIN_I2C_SCL     11
#define PIN_ACC_INT1    14
#define PIN_ACC_INT2    13

#define PIN_VIB_MOTOR   17
#define PIN_BATT_ADC     9
#define PIN_CHRG_STAT   10
#define PIN_USB_DET     21

#define DISP_W         200
#define DISP_H         200
