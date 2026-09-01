/**
 * Bring-up only: seeds example complications onto the store directly,
 * bypassing the not-yet-implemented install path (atelier push ->
 * net_svc.c -> kb_store_install(), all still TODO). Writes
 * /apps/<id>/app.qjb from an embedded bytecode array if it isn't already
 * there; kb_store_read_manifest()'s bring-up stub already expects
 * exactly this layout ("permissive defaults so a hand-copied app
 * boots" - see app_store.c).
 *
 * kb_store_list()/the launcher only ever boot the first app found
 * (readdir order, "last app" selection is a roadmap TODO) - seed
 * exactly one at a time for a predictable test, not several.
 *
 * Remove this file + its call in main.c once a real install path
 * exists (roadmap: app_store untar/manifest/HMAC, then the sync
 * endpoint) and can seed apps for real.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "hello_bytecode.h"
#include "budget_hog_bytecode.h"
#include "seed_apps.h"

static const char *TAG = "seed_apps";

static void seed(const char *id, const uint8_t *bc, size_t len) {
    char dir[80], path[96];
    snprintf(dir, sizeof dir, "/apps/%s", id);
    snprintf(path, sizeof path, "/apps/%s/app.qjb", id);

    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "%s already seeded (%ld bytes)", id, (long)st.st_size);
        return;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", dir, strerror(errno));
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed: %s", path, strerror(errno));
        return;
    }
    size_t n = fwrite(bc, 1, len, f);
    fclose(f);
    if (n != len) {
        ESP_LOGE(TAG, "%s: short write %u/%u bytes", id, (unsigned)n, (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "seeded %s (%u bytes)", path, (unsigned)n);
}

void seed_hello_app(void) {
    seed("hello", k_hello_qjb, sizeof k_hello_qjb);
}

void seed_budget_hog_app(void) {
    seed("budget-hog", k_budget_hog_qjb, sizeof k_budget_hog_qjb);
}
