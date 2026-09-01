/**
 * Bring-up only: seeds the "hello" example complication onto the store
 * directly, bypassing the not-yet-implemented install path (atelier
 * push -> net_svc.c -> kb_store_install(), all still TODO). Writes
 * /apps/hello/app.qjb from the embedded bytecode if it isn't already
 * there; kb_store_read_manifest()'s bring-up stub already expects
 * exactly this layout ("permissive defaults so a hand-copied app
 * boots" - see app_store.c).
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
#include "seed_hello.h"

static const char *TAG = "seed_hello";

void seed_hello_app(void) {
    struct stat st;
    if (stat("/apps/hello/app.qjb", &st) == 0) {
        ESP_LOGI(TAG, "hello already seeded (%ld bytes)", (long)st.st_size);
        return;
    }
    if (mkdir("/apps/hello", 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir /apps/hello failed: %s", strerror(errno));
        return;
    }
    FILE *f = fopen("/apps/hello/app.qjb", "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen app.qjb failed: %s", strerror(errno));
        return;
    }
    size_t n = fwrite(k_hello_qjb, 1, sizeof k_hello_qjb, f);
    fclose(f);
    if (n != sizeof k_hello_qjb) {
        ESP_LOGE(TAG, "short write: %u/%u bytes", (unsigned)n, (unsigned)sizeof k_hello_qjb);
        return;
    }
    ESP_LOGI(TAG, "seeded /apps/hello/app.qjb (%u bytes)", (unsigned)n);
}
