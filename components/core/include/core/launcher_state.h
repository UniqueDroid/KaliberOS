/**
 * Kaliber core — launcher state (docs/design/launcher-states.md).
 *
 * WATCHFACE (base state) / MENU / APP - see the design doc for the full
 * model. This module only owns persistence (NVS, same get-or-default
 * shape as app_store.c's get_hmac_key()/net_svc.c's ap_password()); the
 * transition logic and wake-cause revert rule live in launcher.c, which
 * is the only caller.
 *
 * Only WATCHFACE and MENU are ever actually read back from NVS at boot -
 * APP always reverts to WATCHFACE before a deep sleep (design doc §1),
 * so it's never the persisted value a fresh boot finds; kept as a real
 * enum value anyway (not a bool) so a future state doesn't need a
 * format change.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "core/app_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KB_LSTATE_WATCHFACE,
    KB_LSTATE_MENU,
    KB_LSTATE_APP,
} kb_launcher_state_t;

/* Defaults to KB_LSTATE_WATCHFACE if nothing was ever persisted. */
kb_launcher_state_t kb_launcher_state_get(void);
esp_err_t            kb_launcher_state_set(kb_launcher_state_t state);

/* The watchface to show in WATCHFACE state - "choose a watchface" (menu
 * design doc §3) writes this. ESP_ERR_NOT_FOUND if nothing was ever
 * selected (design doc §2: caller falls back to the first installed
 * type==watchface package, then the existing NO-APPS screen). */
esp_err_t kb_launcher_active_face_get(char out_id[KB_APP_ID_MAX]);
esp_err_t kb_launcher_active_face_set(const char *id);

#ifdef __cplusplus
}
#endif
