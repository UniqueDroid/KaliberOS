#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "core/launcher_state.h"

static const char *TAG = "launcher_state";

#define NVS_NAMESPACE "kaliber_launcher"
#define NVS_KEY_STATE "state"
#define NVS_KEY_FACE  "face"

kb_launcher_state_t kb_launcher_state_get(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return KB_LSTATE_WATCHFACE;
    uint8_t v = KB_LSTATE_WATCHFACE;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_STATE, &v);
    nvs_close(h);
    if (err != ESP_OK || v > KB_LSTATE_APP) return KB_LSTATE_WATCHFACE;
    return (kb_launcher_state_t)v;
}

esp_err_t kb_launcher_state_set(kb_launcher_state_t state) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_STATE, (uint8_t)state);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t kb_launcher_active_face_get(char out_id[KB_APP_ID_MAX]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = KB_APP_ID_MAX;
    err = nvs_get_str(h, NVS_KEY_FACE, out_id, &len);
    nvs_close(h);
    return err;
}

esp_err_t kb_launcher_active_face_set(const char *id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_FACE, id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "active face -> '%s'", id);
    return err;
}
