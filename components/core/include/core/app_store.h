/**
 * Kaliber core — complication store.
 *
 * Layout on the LittleFS partition mounted at /apps:
 *
 *   /apps/<id>/manifest.json
 *   /apps/<id>/app.qjb          (QuickJS bytecode)
 *   /apps/<id>/app.mqb          (MQuickJS bytecode)
 *   /apps/.staging/...          (atomic install: write here, then rename)
 *   /apps/<id>/state.json       (last onSuspend() snapshot)
 *
 * Install is atomic: unpack .comp into .staging/<id>, validate manifest +
 * ABI + HMAC, then rename over the target directory.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KB_APP_ID_MAX      64
#define KB_APP_ABI_VERSION 1   /* bump when engine bytecode format changes  */

typedef enum { KB_APP_WATCHFACE, KB_APP_APP } kb_app_type_t;

typedef struct {
    char          id[KB_APP_ID_MAX];
    char          version[16];
    kb_app_type_t type;
    uint32_t      abi;
    bool          perm_net;
    bool          perm_storage;
    bool          perm_sensors;
    /* entry file for the engine this board runs, resolved at load time */
    char          entry[32];
} kb_manifest_t;

esp_err_t kb_store_init(void);

/* Install a .comp package from a memory buffer (e.g. HTTP upload). */
esp_err_t kb_store_install(const uint8_t *pkg, size_t len, char out_id[KB_APP_ID_MAX]);

esp_err_t kb_store_remove(const char *id);

/* Iterate installed complications. Returns count, fills ids up to max. */
int kb_store_list(char ids[][KB_APP_ID_MAX], int max);

esp_err_t kb_store_read_manifest(const char *id, kb_manifest_t *out);

/* Load engine bytecode for app <id>. Caller frees *out with free(). */
esp_err_t kb_store_read_bytecode(const char *id, uint8_t **out, size_t *len);

/* Persisted app state (JSON string), NULL if none. Caller frees. */
char     *kb_store_read_state(const char *id);
esp_err_t kb_store_write_state(const char *id, const char *json);

#ifdef __cplusplus
}
#endif
