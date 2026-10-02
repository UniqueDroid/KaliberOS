/* Host-build stand-in for ESP-IDF's esp_err.h - just enough surface for
 * gfx/cadran (board_hal/board.h, cadran/cadran.h, loader.c/render.c/
 * providers.c) to compile unmodified on the host. See
 * tools/host_render/README at the top of host_render.c for why this
 * exists at all. */
#pragma once

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_VERSION 0x110

static inline const char *esp_err_to_name(esp_err_t e) {
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
    default: return "ESP_ERR_UNKNOWN";
    }
}
