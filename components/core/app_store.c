/**
 * Complication store — LittleFS at /apps.
 *
 * .comp package format (see tools/atelier/atelier.py): plain uncompressed
 * USTAR tar, flat (no subdirectories) - manifest.json, app.qjb and/or
 * app.mqb, sig.hmac (hex HMAC-SHA256 over manifest.json + the bytecode
 * entries, sorted by name - exactly atelier's signing order). Unsigned
 * packages are rejected outright: an unverified package is code
 * execution with full privileges. Verification happens before any of
 * the package's *content* is trusted (JSON-parsed, used to build a
 * path, written to flash) - the low-level tar scan below still has to
 * run first to even find the byte ranges to hash, but it only measures
 * structure, it doesn't act on anything it finds.
 *
 * HMAC key: per-device, generated on first boot and stored in NVS (see
 * get_hmac_key()) - never a fixed value baked into the firmware image.
 * A key that ships in source control/sdkconfig.defaults stops being a
 * secret the moment the repo is pushed; this repo is public. Overridable
 * via Kconfig (KALIBER_STORE_HMAC_KEY_OVERRIDE) only for pre-provisioning
 * a fleet with a shared key before flashing - not for casual use.
 *
 * Install unpacks (post-verification) into /apps/.staging/<id>, validates
 * the manifest, then swaps it into place over /apps/<id>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "core/app_store.h"
#include "core/event_bus.h"
#include "store_install_selftest_pkg.h"

static const char *TAG = "store";
#define ROOT "/apps"
#define STAGING ROOT "/.staging"

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

/* --------------------------------------------------------------- untar */

/* Minimal USTAR reader: only what atelier's flat, uncompressed packages
 * need (regular files at the top level, no long-name/long-link extension
 * headers, no subdirectories). Not a general tar implementation. */
typedef struct {
    char           name[101];
    size_t         size;
    const uint8_t *data;
} tar_entry_t;

static size_t oct_to_size(const char *field, size_t len) {
    size_t v = 0;
    for (size_t i = 0; i < len && field[i]; i++) {
        if (field[i] < '0' || field[i] > '7') break;
        v = v * 8 + (size_t)(field[i] - '0');
    }
    return v;
}

/* Parses pkg into entries[], up to max entries. Returns count, or -1 on a
 * structurally broken tar (used defensively - this buffer came over
 * HTTP/serial, not from a trusted local build). */
static int tar_parse(const uint8_t *pkg, size_t len, tar_entry_t *entries, int max) {
    int n = 0;
    size_t off = 0;
    while (off + 512 <= len && n < max) {
        const uint8_t *hdr = pkg + off;
        bool all_zero = true;
        for (int i = 0; i < 512; i++) if (hdr[i]) { all_zero = false; break; }
        if (all_zero) break; /* end-of-archive marker */

        char name[101];
        memcpy(name, hdr, 100);
        name[100] = '\0';
        if (name[0] == '\0') break;

        char typeflag = (char)hdr[156];
        size_t fsize = oct_to_size((const char *)hdr + 124, 12);
        size_t data_off = off + 512;
        if (data_off + fsize > len) {
            ESP_LOGE(TAG, "tar: entry '%s' claims %u bytes past end of package", name, (unsigned)fsize);
            return -1;
        }

        /* '0' and '\0' both mean "regular file" per the USTAR spec - only
         * kind atelier ever emits, directories/etc. are skipped. */
        if (typeflag == '0' || typeflag == '\0') {
            strlcpy(entries[n].name, name, sizeof entries[n].name);
            entries[n].size = fsize;
            entries[n].data = pkg + data_off;
            n++;
        }

        off = data_off + ((fsize + 511) / 512) * 512;
    }
    return n;
}

static const tar_entry_t *tar_find(const tar_entry_t *entries, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

/* --------------------------------------------------------------- hmac */

static bool hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    if (strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        unsigned hi, lo;
        if (sscanf(&hex[i * 2], "%1x", &hi) != 1) return false;
        if (sscanf(&hex[i * 2 + 1], "%1x", &lo) != 1) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

#define NVS_NAMESPACE "kaliber_store"
#define NVS_KEY_NAME  "hmac_key"

/* Per-device HMAC key: Kconfig override first (fleet pre-provisioning),
 * else NVS - reading an existing key, or generating+persisting a fresh
 * random one on first call. The generated key is logged in full on
 * purpose: it's the provisioning step itself (copy it into
 * `atelier.py pack --key ...`), a pairing code, not a secret meant to
 * stay hidden from whoever holds the device serial console. */
static esp_err_t get_hmac_key(uint8_t key[32]) {
    if (strlen(CONFIG_KALIBER_STORE_HMAC_KEY_OVERRIDE) == 64 &&
        hex_decode(CONFIG_KALIBER_STORE_HMAC_KEY_OVERRIDE, key, 32)) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t klen = 32;
    err = nvs_get_blob(h, NVS_KEY_NAME, key, &klen);
    if (err == ESP_OK && klen == 32) { nvs_close(h); return ESP_OK; }

    esp_fill_random(key, 32);
    err = nvs_set_blob(h, NVS_KEY_NAME, key, 32);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", key[i]);
    ESP_LOGW(TAG, "generated new app-store HMAC key - for 'atelier.py pack --key ...': %s", hex);
    return ESP_OK;
}

typedef struct { const uint8_t *data; size_t len; } buf_seg_t;

/* Verifies sig_hex (ASCII hex HMAC-SHA256) over the concatenation of
 * segs[0..n_segs), fed incrementally rather than copied into one
 * contiguous buffer first - the segments already live at various offsets
 * inside the received package.
 *
 * Uses the PSA Crypto API, not mbedtls_md_hmac_*(): this mbedtls version
 * (TF-PSA-Crypto, v4) only declares the direct md-HMAC functions under
 * MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS - they're not public API here
 * anymore, PSA is (found the hard way, first build attempt used them and
 * failed to link against a header that doesn't declare them without that
 * macro). psa_mac_verify_finish() also does the constant-time comparison
 * itself, no separate mbedtls_ct_memcmp() call needed. */
static bool verify_hmac_segments(const buf_seg_t *segs, int n_segs,
                                  const uint8_t *key, size_t key_len,
                                  const char *sig_hex, size_t sig_hex_len) {
    uint8_t want[32];
    if (sig_hex_len != 64 || !hex_decode(sig_hex, want, 32)) return false; /* SHA-256 = 32 B = 64 hex chars */

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS && st != PSA_ERROR_ALREADY_EXISTS) return false;

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);

    mbedtls_svc_key_id_t key_id;
    if (psa_import_key(&attrs, key, key_len, &key_id) != PSA_SUCCESS) return false;

    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    bool step_ok = psa_mac_verify_setup(&op, key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS;
    for (int i = 0; step_ok && i < n_segs; i++)
        step_ok = psa_mac_update(&op, segs[i].data, segs[i].len) == PSA_SUCCESS;

    /* Bug found during the 2026-09-04 deadlock audit: psa_mac_verify_finish()
     * failing (PSA_ERROR_INVALID_SIGNATURE on a tampered/wrong-key package -
     * the routine, expected outcome of the tamper test below, not a rare
     * edge case) was falling through without an abort. Per the PSA spec,
     * ANY non-successful completion of an operation (setup/update/finish)
     * needs an explicit psa_mac_abort() - only a *successful*
     * verify_finish() ends the operation on its own. Whether a leaked
     * operation context here can explain the mkdir() hang is exactly what
     * this session's step 1/2 probes are checking. */
    bool verified;
    if (step_ok) {
        verified = psa_mac_verify_finish(&op, want, sizeof want) == PSA_SUCCESS;
        if (!verified) psa_mac_abort(&op);
    } else {
        verified = false;
        psa_mac_abort(&op);
    }
    psa_destroy_key(key_id);
    return verified;
}

static int cmp_tar_entry_name(const void *a, const void *b) {
    return strcmp(((const tar_entry_t *)a)->name, ((const tar_entry_t *)b)->name);
}

/* -------------------------------------------------------------- paths */

static char *path_of(const char *id, const char *file) {
    static char p[160];
    snprintf(p, sizeof p, ROOT "/%s/%s", id, file);
    return p;
}

static bool valid_id(const char *id) {
    if (!id || !id[0] || strlen(id) >= KB_APP_ID_MAX) return false;
    /* No path traversal - id becomes a literal directory name under
     * ROOT/STAGING, and it came out of an untrusted package. */
    for (const char *p = id; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return false;
    return true;
}

/* Same rule as valid_id(), for the bytecode filename the manifest names
 * (e.g. "app.qjb") - it ends up in a path too (staging_dir/<filename>).
 * The HMAC check above already covers tampering by anyone without the
 * key, but this is defense in depth against a bug in atelier itself (or
 * a future manifest field) producing a bad filename, not just against
 * attackers - same reasoning as Cadran's str_ref NUL-termination check. */
static bool valid_filename(const char *name) {
    if (!name || !name[0] || strlen(name) >= 64) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* Recursively removes dir and everything in it. mkdir()/rmdir()/unlink()
 * all come from esp_littlefs's VFS registration (esp_vfs_littlefs_register
 * in kb_store_init()) - same POSIX surface as the fopen()/DIR* calls
 * already used elsewhere in this file. */
static esp_err_t rm_recursive(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return ESP_OK; /* nothing to remove */
    struct dirent *e;
    char child[384]; /* generous: GCC's format-truncation check assumes
                       * d_name could be up to NAME_MAX (255) regardless
                       * of what littlefs filenames actually look like
                       * here. */
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(child, sizeof child, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) rm_recursive(child);
        else unlink(child);
    }
    closedir(d);
    return rmdir(dir) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kb_store_remove(const char *id) {
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    char dir[128];
    snprintf(dir, sizeof dir, ROOT "/%s", id);
    return rm_recursive(dir);
}

/* ------------------------------------------------------------- install */

esp_err_t kb_store_install(const uint8_t *pkg, size_t len, char out_id[KB_APP_ID_MAX]) {
    if (!pkg || !len) return ESP_ERR_INVALID_ARG;

    tar_entry_t entries[8];
    int n = tar_parse(pkg, len, entries, 8);
    if (n <= 0) {
        ESP_LOGE(TAG, "install: bad or empty tar");
        return ESP_ERR_INVALID_ARG;
    }

    const tar_entry_t *mf_entry = tar_find(entries, n, "manifest.json");
    const tar_entry_t *sig_entry = tar_find(entries, n, "sig.hmac");
    if (!mf_entry) {
        ESP_LOGE(TAG, "install: no manifest.json in package");
        return ESP_ERR_INVALID_ARG;
    }
    if (!sig_entry) {
        ESP_LOGE(TAG, "install: no sig.hmac in package - unsigned packages are rejected");
        return ESP_ERR_INVALID_ARG;
    }

    /* --- verify BEFORE trusting anything about the content ------------
     * Signed payload = manifest.json bytes + every other entry (i.e. the
     * bytecode files), sorted by name - exactly atelier.py's cmd_pack
     * signing order. This is purely structural (tar entry names/offsets,
     * not their JSON content), so it can happen before any JSON parsing
     * or path construction touches anything the package supplied. */
    tar_entry_t others[8];
    int n_others = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].data == mf_entry->data || entries[i].data == sig_entry->data) continue;
        others[n_others++] = entries[i];
    }
    qsort(others, n_others, sizeof others[0], cmp_tar_entry_name);

    buf_seg_t segs[9];
    segs[0] = (buf_seg_t){ mf_entry->data, mf_entry->size };
    for (int i = 0; i < n_others; i++) segs[i + 1] = (buf_seg_t){ others[i].data, others[i].size };

    char sig_hex[65];
    size_t sig_len = sig_entry->size < sizeof sig_hex - 1 ? sig_entry->size : sizeof sig_hex - 1;
    memcpy(sig_hex, sig_entry->data, sig_len);
    sig_hex[sig_len] = '\0';

    uint8_t key[32];
    if (get_hmac_key(key) != ESP_OK) {
        ESP_LOGE(TAG, "install: could not obtain HMAC key");
        return ESP_FAIL;
    }
    if (!verify_hmac_segments(segs, n_others + 1, key, sizeof key, sig_hex, sig_len)) {
        ESP_LOGE(TAG, "install: HMAC verification failed - rejecting package");
        return ESP_ERR_INVALID_ARG;
    }

    /* --- signature verified; safe to act on the content now ---------- */
    /* cJSON_ParseWithLength needs a NUL-terminated-or-length-bounded
     * buffer; the tar entry points into pkg directly (not NUL-terminated
     * at its own end - the next entry's header follows immediately), so
     * this must use the length-bounded parse, not cJSON_Parse(). */
    cJSON *mf = cJSON_ParseWithLength((const char *)mf_entry->data, mf_entry->size);
    if (!mf) {
        ESP_LOGE(TAG, "install: manifest.json is not valid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_INVALID_ARG;
    const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(mf, "id");
    const cJSON *j_abi = cJSON_GetObjectItemCaseSensitive(mf, "abi");
    const cJSON *j_entries = cJSON_GetObjectItemCaseSensitive(mf, "entries");
    if (!cJSON_IsString(j_id) || !valid_id(j_id->valuestring)) {
        ESP_LOGE(TAG, "install: manifest 'id' missing or invalid");
        goto out;
    }
    if (!cJSON_IsNumber(j_abi) || (uint32_t)j_abi->valuedouble != KB_APP_ABI_VERSION) {
        ESP_LOGE(TAG, "install: abi mismatch: pkg=%s fw=%d",
                 j_abi ? cJSON_Print(j_abi) : "(missing)", KB_APP_ABI_VERSION);
        goto out;
    }
    if (!cJSON_IsObject(j_entries)) {
        ESP_LOGE(TAG, "install: manifest has no 'entries' object");
        goto out;
    }

    /* Which entry (bytecode filename) this board's engine needs. */
    const char *engine_key = board_get()->caps.engine == KB_ENGINE_QUICKJS ? "quickjs" : "mquickjs";
    const cJSON *j_file = cJSON_GetObjectItemCaseSensitive(j_entries, engine_key);
    if (!cJSON_IsString(j_file) || !valid_filename(j_file->valuestring)) {
        ESP_LOGE(TAG, "install: package has no valid '%s' entry for this board's engine", engine_key);
        goto out;
    }
    const tar_entry_t *bc_entry = tar_find(entries, n, j_file->valuestring);
    if (!bc_entry) {
        ESP_LOGE(TAG, "install: manifest references '%s', not present in package", j_file->valuestring);
        goto out;
    }

    /* --- structural validation passed; unpack into staging ------------ */
    const char *id = j_id->valuestring;
    char staging_dir[128];
    snprintf(staging_dir, sizeof staging_dir, STAGING "/%s", id);

    /* Deadlock-audit step 1 (2026-09-04): if the untar code overran a
     * buffer, this is where it would show up - before touching the flash
     * at all, so a corrupt heap isn't confused with the mkdir() hang
     * itself. Kept permanently, not just for this probe: cheap, and
     * "heap looked fine going into littlefs" is a useful fact either way. */
    bool heap_ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(TAG, "pre-mkdir: heap_ok=%s free=%u largest_block=%u",
             heap_ok ? "yes" : "NO(!)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    mkdir(STAGING, 0755); /* ok if it already exists */
    rm_recursive(staging_dir); /* clear any leftover from a prior failed install */
    if (mkdir(staging_dir, 0755) != 0) {
        ESP_LOGE(TAG, "install: mkdir '%s' failed", staging_dir);
        result = ESP_FAIL;
        goto out;
    }

    bool write_ok = true;
    {
        char p[160];
        snprintf(p, sizeof p, "%s/manifest.json", staging_dir);
        FILE *f = fopen(p, "wb");
        if (!f || fwrite(mf_entry->data, 1, mf_entry->size, f) != mf_entry->size) write_ok = false;
        if (f) fclose(f);
    }
    {
        char p[160];
        snprintf(p, sizeof p, "%s/%s", staging_dir, j_file->valuestring);
        FILE *f = fopen(p, "wb");
        if (!f || fwrite(bc_entry->data, 1, bc_entry->size, f) != bc_entry->size) write_ok = false;
        if (f) fclose(f);
    }
    if (!write_ok) {
        ESP_LOGE(TAG, "install: write to staging failed");
        rm_recursive(staging_dir);
        result = ESP_FAIL;
        goto out;
    }

    /* Swap into place. Not fully atomic across a power loss mid-swap (the
     * old version is gone before the new one lands) - acceptable for now,
     * flagged rather than silently assumed: a true atomic swap needs
     * either a rename-over-nonempty-dir that littlefs doesn't support, or
     * a level of indirection (e.g. a "current version" pointer file) this
     * store doesn't have yet. */
    char final_dir[128];
    snprintf(final_dir, sizeof final_dir, ROOT "/%s", id);
    rm_recursive(final_dir);
    if (rename(staging_dir, final_dir) != 0) {
        ESP_LOGE(TAG, "install: rename staging -> '%s' failed", final_dir);
        rm_recursive(staging_dir);
        result = ESP_FAIL;
        goto out;
    }

    strlcpy(out_id, id, KB_APP_ID_MAX);
    ESP_LOGI(TAG, "install: '%s' ok (%s, %u B bytecode)", id, engine_key, (unsigned)bc_entry->size);

    event_t ev = { .type = EV_APP_INSTALLED, .payload = strdup(id) };
    if (!ev.payload || kb_bus_post(&ev) != ESP_OK) free(ev.payload);

    result = ESP_OK;
out:
    cJSON_Delete(mf);
    return result;
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
    memset(out, 0, sizeof *out);
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path_of(id, "manifest.json"), "rb");
    if (!f) {
        /* Bring-up fallback: seed_apps.c writes bytecode straight into
         * /apps/<id>/ with no manifest.json (see its header comment) -
         * degrade to the old permissive defaults instead of failing, so
         * hello/budget-hog keep booting without needing atelier. A real
         * installed app (via kb_store_install(), always writes a
         * manifest) takes the branch below instead. */
        strlcpy(out->id, id, sizeof out->id);
        out->abi = KB_APP_ABI_VERSION;
        out->type = KB_APP_WATCHFACE;
        strlcpy(out->entry, "app.qjb", sizeof out->entry);
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz);
    bool read_ok = buf && fread(buf, 1, sz, f) == (size_t)sz;
    fclose(f);
    if (!read_ok) { free(buf); return ESP_FAIL; }

    cJSON *mf = cJSON_ParseWithLength(buf, sz);
    free(buf);
    if (!mf) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(mf, "id");
    const cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(mf, "version");
    const cJSON *j_type = cJSON_GetObjectItemCaseSensitive(mf, "type");
    const cJSON *j_abi = cJSON_GetObjectItemCaseSensitive(mf, "abi");
    const cJSON *j_entries = cJSON_GetObjectItemCaseSensitive(mf, "entries");
    if (!cJSON_IsString(j_id) || !cJSON_IsNumber(j_abi) || !cJSON_IsObject(j_entries)) goto done;

    strlcpy(out->id, j_id->valuestring, sizeof out->id);
    if (cJSON_IsString(j_ver)) strlcpy(out->version, j_ver->valuestring, sizeof out->version);
    out->type = (cJSON_IsString(j_type) && strcmp(j_type->valuestring, "app") == 0)
                    ? KB_APP_APP : KB_APP_WATCHFACE;
    out->abi = (uint32_t)j_abi->valuedouble;

    const char *engine_key = board_get()->caps.engine == KB_ENGINE_QUICKJS ? "quickjs" : "mquickjs";
    const cJSON *j_file = cJSON_GetObjectItemCaseSensitive(j_entries, engine_key);
    if (!cJSON_IsString(j_file)) goto done;
    strlcpy(out->entry, j_file->valuestring, sizeof out->entry);

    const cJSON *j_perms = cJSON_GetObjectItemCaseSensitive(mf, "permissions");
    if (cJSON_IsArray(j_perms)) {
        const cJSON *p;
        cJSON_ArrayForEach(p, j_perms) {
            if (!cJSON_IsString(p)) continue;
            if (strcmp(p->valuestring, "net") == 0) out->perm_net = true;
            else if (strcmp(p->valuestring, "storage") == 0) out->perm_storage = true;
            else if (strcmp(p->valuestring, "sensors") == 0) out->perm_sensors = true;
        }
    }
    err = ESP_OK;
done:
    cJSON_Delete(mf);
    return err;
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

/* ------------------------------------------------------------ selftest */

/* Bring-up harness for kb_store_install(), same rationale as
 * cadran_selftest()/js_watchface_selftest() - "notfalls über ein
 * eingebettetes Testpaket" (project chat 2026-09-03): no net_svc.c yet
 * to actually deliver a package over HTTP, so a real .comp built by
 * atelier.py (examples/complications/hello, signed with the test key
 * KALIBER_STORE_HMAC_KEY_OVERRIDE must be set to for this to pass) is
 * embedded and installed directly. Exercises the full path: HMAC verify
 * -> unpack -> manifest validate -> stage -> swap into place -> list/
 * read-manifest/read-bytecode round-trip - then a tampered copy to
 * confirm rejection, and removes what it installed either way so it
 * doesn't race seed_hello_app()'s separate "hello" id for which app the
 * launcher boots (see main.c's seed_hello_app() comment). Temporary,
 * remove alongside the other bring-up selftests once there's a real
 * install path exercising this (net_svc.c). */
void kb_store_install_selftest(void) {
    char id[KB_APP_ID_MAX];
    esp_err_t err = kb_store_install(k_hello_comp, sizeof k_hello_comp, id);
    bool install_ok = (err == ESP_OK) && strcmp(id, "de.jan.hello") == 0;
    if (!install_ok) {
        ESP_LOGE(TAG, "FAIL: install: %s (id='%s')", esp_err_to_name(err), install_ok ? id : "?");
        return;
    }

    char ids[8][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 8);
    bool listed = false;
    for (int i = 0; i < n; i++) if (strcmp(ids[i], id) == 0) listed = true;

    kb_manifest_t mf;
    bool mf_ok = kb_store_read_manifest(id, &mf) == ESP_OK
                 && mf.abi == KB_APP_ABI_VERSION
                 && strcmp(mf.entry, "app.qjb") == 0;

    uint8_t *bc = NULL;
    size_t bclen = 0;
    bool bc_ok = kb_store_read_bytecode(id, &bc, &bclen) == ESP_OK && bclen > 0;
    free(bc);

    kb_store_remove(id); /* clean up before the tamper test re-installs the same id */

    /* Flip one byte inside the bytecode entry (well past the tar header,
     * inside app.qjb's content) - same package otherwise, must now fail
     * HMAC verification and install nothing. */
    uint8_t tampered[sizeof k_hello_comp];
    memcpy(tampered, k_hello_comp, sizeof tampered);
    tampered[600] ^= 0xff;
    char tamper_id[KB_APP_ID_MAX] = {0};
    esp_err_t tamper_err = kb_store_install(tampered, sizeof tampered, tamper_id);
    bool tamper_rejected = (tamper_err != ESP_OK);
    kb_store_remove("de.jan.hello"); /* in case rejection somehow still wrote something */

    bool pass = install_ok && listed && mf_ok && bc_ok && tamper_rejected;
    ESP_LOGI(TAG, "%s: install=ok listed=%s manifest=%s bytecode=%s(%uB) tamper_rejected=%s",
             pass ? "PASS" : "FAIL",
             listed ? "yes" : "no", mf_ok ? "ok" : "bad", bc_ok ? "ok" : "bad",
             (unsigned)bclen, tamper_rejected ? "yes" : "NO(!)");
}

/* ------------------------------------------------------- deadlock probe */

/* Step 2 of the 2026-09-04 deadlock audit ("halbieren"): the real
 * manifest content, parsed the same way kb_store_install() does, and a
 * real (dummy-key) HMAC verify over comparable-sized data, run in
 * isolation from each other, each immediately followed by a mkdir() on
 * its own scratch directory. mode 0 = JSON only, 1 = HMAC only, 2 = both
 * (the combination that hangs in kb_store_install() itself) - run one at
 * a time, not all three back to back, so a hang in an earlier mode
 * doesn't taint a later one's result. Triggered on demand from the
 * console (see main.c's console_task()), never at boot - a repeat of
 * this hang must not boot-loop the device. */
void kb_store_deadlock_probe(int mode) {
    static const char *k_dummy_manifest =
        "{\"id\":\"de.jan.hello\",\"version\":\"0.1.0\",\"type\":\"watchface\","
        "\"abi\":1,\"entries\":{\"quickjs\":\"app.qjb\"},\"permissions\":[]}";

    if (mode == 0 || mode == 2) {
        cJSON *mf = cJSON_ParseWithLength(k_dummy_manifest, strlen(k_dummy_manifest));
        ESP_LOGI(TAG, "probe: JSON parse %s", mf ? "ok" : "FAILED");
        cJSON_Delete(mf);
    }

    if (mode == 1 || mode == 2) {
        uint8_t dummy_key[32] = {0};
        buf_seg_t segs[1] = {{ (const uint8_t *)k_dummy_manifest, strlen(k_dummy_manifest) }};
        /* Deliberately not a real signature - result doesn't matter here,
         * only whether the PSA calls leave something behind. */
        bool r = verify_hmac_segments(segs, 1, dummy_key, sizeof dummy_key,
            "0000000000000000000000000000000000000000000000000000000000000", 64);
        ESP_LOGI(TAG, "probe: HMAC verify returned %s (expected false, dummy sig)", r ? "true" : "false");
    }

    bool heap_ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(TAG, "probe mode=%d pre-mkdir: heap_ok=%s free=%u largest_block=%u",
             mode, heap_ok ? "yes" : "NO(!)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    char dir[48];
    snprintf(dir, sizeof dir, ROOT "/.probe_%d", mode);
    rm_recursive(dir);
    int64_t t0 = esp_timer_get_time();
    errno = 0;
    int r = mkdir(dir, 0755);
    ESP_LOGI(TAG, "probe mode=%d: mkdir r=%d errno=%d took %lld us",
             mode, r, errno, (long long)(esp_timer_get_time() - t0));
    rm_recursive(dir);
}
