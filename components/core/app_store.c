/**
 * Complication store — LittleFS at /apps.
 * Skeleton: mount + list + raw file IO implemented; .comp unpack (untar),
 * manifest JSON parsing (cJSON) and HMAC check are TODO and clearly marked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "core/app_store.h"

static const char *TAG = "store";
#define ROOT "/apps"

esp_err_t kb_store_init(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = ROOT,
        .partition_label = "apps",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        size_t total = 0, used = 0;
        esp_littlefs_info(conf.partition_label, &total, &used);
        ESP_LOGI(TAG, "mounted, %u/%u kB used",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    return err;
}

esp_err_t kb_store_install(const uint8_t *pkg, size_t len,
                           char out_id[KB_APP_ID_MAX]) {
    (void)pkg; (void)len; (void)out_id;
    /* TODO: untar into /apps/.staging/<id>, parse+validate manifest (abi,
     * engine entry exists for board_get()->caps.engine), verify HMAC,
     * rename over /apps/<id>, post EV_APP_INSTALLED. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kb_store_remove(const char *id) {
    (void)id; /* TODO recursive rm */
    return ESP_ERR_NOT_SUPPORTED;
}

int kb_store_list(char ids[][KB_APP_ID_MAX], int max) {
    DIR *d = opendir(ROOT);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.')
            strlcpy(ids[n++], e->d_name, KB_APP_ID_MAX);
    }
    closedir(d);
    return n;
}

esp_err_t kb_store_read_manifest(const char *id, kb_manifest_t *out) {
    (void)id;
    /* TODO: cJSON parse of /apps/<id>/manifest.json.
     * Bring-up stub: permissive defaults so a hand-copied app boots. */
    memset(out, 0, sizeof *out);
    strlcpy(out->id, id, sizeof out->id);
    out->abi = KB_APP_ABI_VERSION;
    out->type = KB_APP_WATCHFACE;
    strlcpy(out->entry, "app.qjb", sizeof out->entry);
    return ESP_OK;
}

static char *path_of(const char *id, const char *file) {
    static char p[160];
    snprintf(p, sizeof p, ROOT "/%s/%s", id, file);
    return p;
}

esp_err_t kb_store_read_bytecode(const char *id, uint8_t **out, size_t *len) {
    kb_manifest_t mf;
    kb_store_read_manifest(id, &mf);
    FILE *f = fopen(path_of(id, mf.entry), "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(sz);
    if (!*out) { fclose(f); return ESP_ERR_NO_MEM; }
    *len = fread(*out, 1, sz, f);
    fclose(f);
    return *len == (size_t)sz ? ESP_OK : ESP_FAIL;
}

char *kb_store_read_state(const char *id) {
    FILE *f = fopen(path_of(id, "state.json"), "rb");
    if (!f) {
        ESP_LOGI(TAG, "read_state(%s): no state.json", id);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(sz + 1);
    if (s) { fread(s, 1, sz, f); s[sz] = 0; }
    fclose(f);
    ESP_LOGI(TAG, "read_state(%s): %s", id, s ? s : "(alloc failed)");
    return s;
}

esp_err_t kb_store_write_state(const char *id, const char *json) {
    FILE *f = fopen(path_of(id, "state.json"), "wb");
    if (!f) return ESP_FAIL;
    fputs(json, f);
    /* Belt and suspenders: fclose() should flush LittleFS's own buffers,
     * but this runs right before esp_deep_sleep_start() - an fsync makes
     * sure nothing is still sitting in a lower-level cache when power
     * effectively drops for the flash controller's purposes. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    ESP_LOGI(TAG, "write_state(%s): %s", id, json);
    return ESP_OK;
}
