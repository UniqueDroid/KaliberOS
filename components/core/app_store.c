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
 * kb_store_install() takes a *path* to the package, not a memory buffer
 * (project chat 2026-09-04, ahead of net_svc.c): a .comp can run to
 * hundreds of KB once it carries real bytecode plus resources, which
 * doesn't fit comfortably in RAM once WiFi's own 50-70 kB stack is also
 * resident. Every step below - tar scan, HMAC verify, staging writes -
 * works off file offsets and small fixed-size chunk buffers, never a
 * buffer sized to the whole package or even a whole entry. net_svc.c
 * streams the HTTP request body straight to this same kind of path.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#include "default_face_pkg.h"

static const char *TAG = "store";
#define ROOT "/apps"
#define STAGING ROOT "/.staging"

/* Every file-to-file/file-to-crypto copy in this file moves data through a
 * buffer this size, never anything sized to a whole entry or package -
 * see the module comment. */
#define CHUNK_SIZE 256

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
 * headers, no subdirectories). Not a general tar implementation. Works
 * off a FILE* + byte offsets, not a memory buffer - only ever reads one
 * 512-byte header at a time, seeking past entry data without touching it. */
typedef struct {
    char   name[101];
    size_t size;
    size_t offset;   /* absolute byte offset of this entry's data in the file */
} tar_entry_t;

static size_t oct_to_size(const char *field, size_t len) {
    size_t v = 0;
    for (size_t i = 0; i < len && field[i]; i++) {
        if (field[i] < '0' || field[i] > '7') break;
        v = v * 8 + (size_t)(field[i] - '0');
    }
    return v;
}

/* Parses the tar at f (total length len) into entries[], up to max
 * entries. Returns count, or -1 on a structurally broken tar (used
 * defensively - this file came over HTTP/serial, not from a trusted
 * local build). */
static int tar_parse(FILE *f, size_t len, tar_entry_t *entries, int max) {
    int n = 0;
    size_t off = 0;
    uint8_t hdr[512];
    while (off + 512 <= len && n < max) {
        if (fseek(f, (long)off, SEEK_SET) != 0 || fread(hdr, 1, 512, f) != 512) {
            ESP_LOGE(TAG, "tar: short read at offset %u", (unsigned)off);
            return -1;
        }
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
            entries[n].offset = data_off;
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

typedef struct { size_t offset; size_t len; } buf_seg_t;

/* Verifies sig_hex (ASCII hex HMAC-SHA256) over the concatenation of
 * segs[0..n_segs), each fed to the MAC incrementally in CHUNK_SIZE
 * pieces read straight off f - never a buffer sized to a whole segment,
 * let alone the whole package (see the module comment: a bytecode entry
 * alone could be well past what fits in RAM once WiFi is also resident).
 *
 * Uses the PSA Crypto API, not mbedtls_md_hmac_*(): this mbedtls version
 * (TF-PSA-Crypto, v4) only declares the direct md-HMAC functions under
 * MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS - they're not public API here
 * anymore, PSA is (found the hard way, first build attempt used them and
 * failed to link against a header that doesn't declare them without that
 * macro). psa_mac_verify_finish() also does the constant-time comparison
 * itself, no separate mbedtls_ct_memcmp() call needed. */
static bool verify_hmac_segments(FILE *f, const buf_seg_t *segs, int n_segs,
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

    uint8_t chunk[CHUNK_SIZE];
    for (int i = 0; step_ok && i < n_segs; i++) {
        size_t pos = segs[i].offset, remaining = segs[i].len;
        while (step_ok && remaining) {
            size_t take = remaining < sizeof chunk ? remaining : sizeof chunk;
            if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(chunk, 1, take, f) != take) {
                step_ok = false;
                break;
            }
            step_ok = psa_mac_update(&op, chunk, take) == PSA_SUCCESS;
            pos += take;
            remaining -= take;
        }
    }

    /* Bug found during the 2026-09-04 deadlock audit: psa_mac_verify_finish()
     * failing (PSA_ERROR_INVALID_SIGNATURE on a tampered/wrong-key package -
     * the routine, expected outcome of the tamper test below, not a rare
     * edge case) was falling through without an abort. Per the PSA spec,
     * ANY non-successful completion of an operation (setup/update/finish)
     * needs an explicit psa_mac_abort() - only a *successful*
     * verify_finish() ends the operation on its own. (Turned out not to be
     * the mkdir() deadlock's actual cause - that was a stack overflow one
     * frame up in the caller, see kb_store_install_selftest() - but this
     * was a real leak in its own right and stays fixed.) */
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

/* Same idea as path_of(), but under an arbitrary directory (e.g. a
 * staging dir) rather than ROOT/<id> - used while unpacking, before the
 * final id-keyed layout exists. */
static char *path_of_in(const char *dir, const char *file) {
    static char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    return p;
}

/* Structural validity only - a "."-prefixed id (kb_store_install_selftest()'s
 * reserved namespace) passes here on purpose, same as any other id. What
 * keeps a real package from claiming one is install_impl()'s separate
 * allow_reserved_id check, right after this one runs - two different
 * concerns (is this a legal directory name vs. who's allowed to use it),
 * deliberately not merged into one. */
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

/* Copies exactly len bytes from src (starting at off) into a new file at
 * dst_path, CHUNK_SIZE bytes at a time - the same "never buffer a whole
 * entry" rule as verify_hmac_segments() above, used for staging the
 * manifest and bytecode entries below. */
static bool copy_range_to_file(FILE *src, size_t off, size_t len, const char *dst_path) {
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) return false;
    bool ok = fseek(src, (long)off, SEEK_SET) == 0;
    uint8_t chunk[CHUNK_SIZE];
    size_t remaining = len;
    while (ok && remaining) {
        size_t take = remaining < sizeof chunk ? remaining : sizeof chunk;
        ok = fread(chunk, 1, take, src) == take && fwrite(chunk, 1, take, dst) == take;
        remaining -= take;
    }
    fclose(dst);
    return ok;
}

/* ------------------------------------------------------------- install */

/* allow_reserved_id: only true for kb_store_install_selftest()'s own two
 * calls (below, same file) - its embedded package is the one legitimate
 * user of the reserved "." prefix (see valid_id() and the reserved-id
 * check just past manifest parsing). Every other caller, in particular
 * net_svc.c's HTTP handler for untrusted network pushes, goes through the
 * kb_store_install() wrapper below with this false, so nothing arriving
 * over the wire can ever claim a reserved id. */
static esp_err_t install_impl(const char *pkg_path, char out_id[KB_APP_ID_MAX],
                               bool allow_reserved_id) {
    if (!pkg_path) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(pkg_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "install: can't open '%s'", pkg_path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long file_len_l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len_l <= 0) {
        ESP_LOGE(TAG, "install: '%s' is empty or unreadable", pkg_path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    size_t file_len = (size_t)file_len_l;

    tar_entry_t entries[8];
    int n = tar_parse(f, file_len, entries, 8);
    if (n <= 0) {
        ESP_LOGE(TAG, "install: bad or empty tar");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    const tar_entry_t *mf_entry = tar_find(entries, n, "manifest.json");
    const tar_entry_t *sig_entry = tar_find(entries, n, "sig.hmac");
    if (!mf_entry) {
        ESP_LOGE(TAG, "install: no manifest.json in package");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    if (!sig_entry) {
        ESP_LOGE(TAG, "install: no sig.hmac in package - unsigned packages are rejected");
        fclose(f);
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
        if (entries[i].offset == mf_entry->offset || entries[i].offset == sig_entry->offset) continue;
        others[n_others++] = entries[i];
    }
    qsort(others, n_others, sizeof others[0], cmp_tar_entry_name);

    buf_seg_t segs[9];
    segs[0] = (buf_seg_t){ mf_entry->offset, mf_entry->size };
    for (int i = 0; i < n_others; i++) segs[i + 1] = (buf_seg_t){ others[i].offset, others[i].size };

    char sig_hex[65];
    size_t sig_len = sig_entry->size < sizeof sig_hex - 1 ? sig_entry->size : sizeof sig_hex - 1;
    if (fseek(f, (long)sig_entry->offset, SEEK_SET) != 0 || fread(sig_hex, 1, sig_len, f) != sig_len) {
        ESP_LOGE(TAG, "install: could not read sig.hmac");
        fclose(f);
        return ESP_FAIL;
    }
    sig_hex[sig_len] = '\0';

    uint8_t key[32];
    if (get_hmac_key(key) != ESP_OK) {
        ESP_LOGE(TAG, "install: could not obtain HMAC key");
        fclose(f);
        return ESP_FAIL;
    }
    if (!verify_hmac_segments(f, segs, n_others + 1, key, sizeof key, sig_hex, sig_len)) {
        ESP_LOGE(TAG, "install: HMAC verification failed - rejecting package");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- signature verified; safe to act on the content now ---------- */
    /* manifest.json is expected small (a few KB at most) - unlike the
     * bytecode/resource entries (streamed below without ever loading a
     * whole one into RAM), cJSON needs its input contiguous, so this one
     * file is read whole into a heap buffer sized exactly to it. */
    char *mf_buf = malloc(mf_entry->size + 1);
    if (!mf_buf) {
        ESP_LOGE(TAG, "install: out of memory reading manifest.json (%u B)", (unsigned)mf_entry->size);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fseek(f, (long)mf_entry->offset, SEEK_SET) != 0 ||
        fread(mf_buf, 1, mf_entry->size, f) != mf_entry->size) {
        ESP_LOGE(TAG, "install: could not read manifest.json");
        free(mf_buf);
        fclose(f);
        return ESP_FAIL;
    }
    mf_buf[mf_entry->size] = '\0';
    cJSON *mf = cJSON_ParseWithLength(mf_buf, mf_entry->size);
    free(mf_buf);
    if (!mf) {
        ESP_LOGE(TAG, "install: manifest.json is not valid JSON");
        fclose(f);
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
    /* Reserved namespace (project chat 2026-09-05): an id starting with
     * "." is kb_store_install_selftest()'s own, never a real package's -
     * kb_store_list() already excludes "."-prefixed entries (readdir()
     * filter below), so this is what makes that exclusion airtight rather
     * than incidental: without this check, a real push could still claim
     * a "."-id, become invisible to the menu/watchface-fallback list, and
     * (worse) collide with whatever the selftest itself is doing that
     * boot. Root cause of a real bug this closes: the selftest used to
     * install/remove the *same* id ("de.jan.hello") the shipped example
     * uses, silently deleting a real install on the next boot. */
    if (!allow_reserved_id && j_id->valuestring[0] == '.') {
        ESP_LOGE(TAG, "install: id '%s' is reserved (leading '.')", j_id->valuestring);
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

    bool write_ok = copy_range_to_file(f, mf_entry->offset, mf_entry->size,
                                        path_of_in(staging_dir, "manifest.json"));
    write_ok = write_ok && copy_range_to_file(f, bc_entry->offset, bc_entry->size,
                                               path_of_in(staging_dir, j_file->valuestring));
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
    /* net_svc.c calls this from its HTTP handler task, whose stack is
     * typically ~4 KB by ESP-IDF default - tighter than main_task's 8 KB
     * that just barely (see the 2026-09-04 mkdir()/stack-overflow postmortem,
     * project chat) survived a caller-side 10 KB local. Logging the actual
     * headroom here, on every real install, means that handler's stack can
     * be sized from measurement instead of found out the hard way again. */
    ESP_LOGI(TAG, "install: caller stack high-water mark %u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    event_t ev = { .type = EV_APP_INSTALLED, .payload = strdup(id) };
    if (!ev.payload || kb_bus_post(&ev) != ESP_OK) free(ev.payload);

    result = ESP_OK;
out:
    cJSON_Delete(mf);
    fclose(f);
    return result;
}

esp_err_t kb_store_install(const char *pkg_path, char out_id[KB_APP_ID_MAX]) {
    return install_impl(pkg_path, out_id, false);
}

int kb_store_list(char ids[][KB_APP_ID_MAX], int max) {
    /* d_name[0] != '.' does double duty: hides littlefs/POSIX dotfiles
     * (there are none today, but readdir() would hand them back same as
     * any other entry) *and* is what keeps kb_store_install_selftest()'s
     * reserved-namespace ids (see install_impl()'s allow_reserved_id)
     * out of the menu/watchface-fallback list - the two purposes share
     * one filter deliberately, not by coincidence. */
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
 * atelier.py, signed with the test key KALIBER_STORE_HMAC_KEY_OVERRIDE
 * must be set to for this to pass, is embedded and installed directly.
 * Exercises the full path: HMAC verify -> unpack -> manifest validate ->
 * stage -> swap into place -> list/read-manifest/read-bytecode round-trip
 * - then a tampered copy to confirm rejection, and removes what it
 * installed either way.
 *
 * The embedded package's manifest id is ".selftest.hello" - its own
 * source, not examples/complications/hello's real "de.jan.hello" (project
 * chat 2026-09-05: it used to reuse that id, and this selftest's own
 * cleanup - kb_store_remove(id) below - silently deleted a real user
 * install under the same id on the next boot; a real bug, not a
 * hypothetical). install_impl()'s allow_reserved_id lets only this
 * function's two calls use a "."-prefixed id at all; kb_store_list()'s
 * dotfile filter keeps it out of the menu/watchface-fallback list the
 * same way it always did.
 *
 * Temporary, remove alongside the other bring-up selftests once there's
 * a real install path exercising this (net_svc.c) - done as of
 * 2026-09-05 (a real push, `atelier push`, round-tripped on hardware) but
 * still here per project chat: safe to keep running as a boot-time
 * regression check now that the id collision is fixed. */
#define KB_STORE_SELFTEST_INSTALL   0x01
#define KB_STORE_SELFTEST_HIDDEN    0x02
#define KB_STORE_SELFTEST_MANIFEST  0x04
#define KB_STORE_SELFTEST_BYTECODE  0x08
#define KB_STORE_SELFTEST_TAMPER    0x10
#define KB_STORE_SELFTEST_ALL_PASS  0x1F

unsigned kb_store_install_selftest(void) {
    const char *pkg_path = ROOT "/.selftest_pkg.comp";
    FILE *pf = fopen(pkg_path, "wb");
    /* k_hello_comp is a flash-resident (.rodata) const array - writing it
     * straight through fwrite() needs no RAM copy of its own. */
    if (!pf || fwrite(k_hello_comp, 1, sizeof k_hello_comp, pf) != sizeof k_hello_comp) {
        if (pf) fclose(pf);
        ESP_LOGE(TAG, "FAIL: install: could not stage test package");
        return 0;
    }
    fclose(pf);

    char id[KB_APP_ID_MAX];
    esp_err_t err = install_impl(pkg_path, id, true);
    bool install_ok = (err == ESP_OK) && strcmp(id, ".selftest.hello") == 0;
    if (!install_ok) {
        ESP_LOGE(TAG, "FAIL: install: %s (id='%s')", esp_err_to_name(err), install_ok ? id : "?");
        unlink(pkg_path);
        return 0;
    }

    /* "hidden", not "listed" (project chat 2026-09-05, alongside the
     * reserved-namespace fix above): kb_store_list() deliberately excludes
     * "."-prefixed ids, so a reserved-namespace package that DID show up
     * here would be the bug, not the other way around - this now checks
     * the hiding itself works, not (as it used to, back when this used
     * the real "de.jan.hello" id) that a normal install is enumerable. */
    char ids[8][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 8);
    bool hidden = true;
    for (int i = 0; i < n; i++) if (strcmp(ids[i], id) == 0) hidden = false;

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
     * HMAC verification and install nothing. Done in place on the
     * already-staged package file - no RAM copy needed at all now that
     * install works from a path, let alone the 10 KB stack array this
     * used to be (root cause of the 2026-09-04 mkdir()/littlefs
     * "deadlock", which was actually a stack overflow one frame up from
     * here - see kb_store_install()'s and verify_hmac_segments()'s
     * comments). */
    /* Where to flip a byte: NOT a hardcoded offset like "600" (the first
     * two cuts of this refactor, 2026-09-04, both used one and both
     * silently "passed" the tamper test - install=ok, tamper_rejected=NO
     * - because that offset turned out to land in unused zero-padding
     * between tar entries, not inside any entry's actual, HMAC-covered
     * bytes: Python's tarfile module (atelier.py's cmd_pack) writes a PAX
     * extended header as the archive's *first* entry, shifting where
     * manifest.json's real content starts well past where a naive
     * "just after the first 512-byte header" guess lands. Re-parsing the
     * already-staged, known-good pkg_path with the exact same tar_parse()/
     * tar_find() the install path itself uses - instead of guessing a
     * number - finds manifest.json's real offset regardless of PAX
     * headers or any other layout detail, the same way the actual
     * verification does. */
    size_t manifest_off = 0, manifest_size = 0;
    {
        FILE *pf2 = fopen(pkg_path, "rb");
        if (pf2) {
            fseek(pf2, 0, SEEK_END);
            long len2 = ftell(pf2);
            tar_entry_t scan[8];
            int n2 = len2 > 0 ? tar_parse(pf2, (size_t)len2, scan, 8) : -1;
            const tar_entry_t *mf2 = n2 > 0 ? tar_find(scan, n2, "manifest.json") : NULL;
            if (mf2) { manifest_off = mf2->offset; manifest_size = mf2->size; }
            fclose(pf2);
        }
    }

    const char *tamper_path = ROOT "/.selftest_tampered.comp";
    FILE *tf = fopen(tamper_path, "wb");
    bool tamper_rejected = false;
    if (!manifest_size) {
        ESP_LOGE(TAG, "FAIL: install: could not locate manifest.json in staged package for tamper test");
        if (tf) fclose(tf);
    } else if (!tf) {
        ESP_LOGE(TAG, "FAIL: install: could not create tamper-test package");
    } else {
        /* A separate, freshly-written file, not an in-place edit of
         * pkg_path - copy k_hello_comp byte for byte through the same
         * CHUNK_SIZE buffer as everywhere else, flipping one byte
         * (manifest_off + manifest_size/2, safely inside its real
         * content) in memory before it's written. */
        size_t tamper_at = manifest_off + manifest_size / 2;
        bool write_ok = true;
        size_t off = 0;
        while (write_ok && off < sizeof k_hello_comp) {
            size_t take = sizeof k_hello_comp - off < CHUNK_SIZE ? sizeof k_hello_comp - off : CHUNK_SIZE;
            uint8_t chunk[CHUNK_SIZE];
            memcpy(chunk, k_hello_comp + off, take);
            if (tamper_at >= off && tamper_at < off + take) chunk[tamper_at - off] ^= 0xff;
            write_ok = fwrite(chunk, 1, take, tf) == take;
            off += take;
        }
        fclose(tf);

        if (!write_ok) {
            ESP_LOGE(TAG, "FAIL: install: could not write tamper-test package");
            unlink(tamper_path);
        } else {
            char tamper_id[KB_APP_ID_MAX] = {0};
            esp_err_t tamper_err = install_impl(tamper_path, tamper_id, true);
            tamper_rejected = (tamper_err != ESP_OK);
            unlink(tamper_path);
        }
        kb_store_remove(".selftest.hello"); /* in case rejection somehow still wrote something */
    }
    unlink(pkg_path);

    bool pass = install_ok && hidden && mf_ok && bc_ok && tamper_rejected;
    ESP_LOGI(TAG, "%s: install=ok hidden=%s manifest=%s bytecode=%s(%uB) tamper_rejected=%s",
             pass ? "PASS" : "FAIL",
             hidden ? "yes" : "NO(!)", mf_ok ? "ok" : "bad", bc_ok ? "ok" : "bad",
             (unsigned)bclen, tamper_rejected ? "yes" : "NO(!)");
    /* Bitmask, not just pass/fail - this line runs at ~1s into boot,
     * squarely inside the post-USB-reset window this project's serial
     * capture has never reliably caught (project chat, all session); the
     * caller re-logs the return value much later (see main.c), and a bare
     * bool there wouldn't say WHICH check failed - cost a whole extra
     * flash/test cycle finding that out live once already (2026-09-04). */
    return (install_ok ? KB_STORE_SELFTEST_INSTALL : 0)
         | (hidden      ? KB_STORE_SELFTEST_HIDDEN  : 0)
         | (mf_ok        ? KB_STORE_SELFTEST_MANIFEST : 0)
         | (bc_ok         ? KB_STORE_SELFTEST_BYTECODE : 0)
         | (tamper_rejected ? KB_STORE_SELFTEST_TAMPER : 0);
}

/* Minimal USTAR header writer - the write-side counterpart to tar_parse()
 * above, needed exactly once (kb_store_install_default_face(), below):
 * everywhere else this firmware only ever reads a package, atelier.py
 * (Python's real tarfile module) writes them. tar_parse() itself never
 * checks the checksum field, but a genuinely spec-compliant archive
 * costs nothing extra here and avoids a surprise if anyone ever
 * inspects one of these with a real `tar tf`. */
static void write_tar_header(FILE *f, const char *name, size_t size) {
    uint8_t hdr[512] = {0};
    strlcpy((char *)hdr, name, 100);
    snprintf((char *)hdr + 100, 8, "%07o", 0644);            /* mode */
    snprintf((char *)hdr + 124, 12, "%011o", (unsigned)size); /* size */
    snprintf((char *)hdr + 136, 12, "%011o", 0);              /* mtime */
    memset(hdr + 148, ' ', 8);                                /* chksum: spaces while summing */
    hdr[156] = '0';                                           /* typeflag: regular file */
    memcpy(hdr + 257, "ustar", 6);                            /* magic, incl. NUL */
    hdr[263] = '0'; hdr[264] = '0';                           /* version "00" */

    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += hdr[i];
    char chksum[8];
    snprintf(chksum, sizeof chksum, "%06o", sum);
    memcpy(hdr + 148, chksum, 6);
    hdr[154] = '\0';
    hdr[155] = ' ';

    fwrite(hdr, 1, 512, f);
}

static void write_tar_entry(FILE *f, const char *name, const uint8_t *data, size_t size) {
    write_tar_header(f, name, size);
    fwrite(data, 1, size, f);
    size_t pad = (512 - (size % 512)) % 512;
    if (pad) {
        uint8_t zeros[512] = {0};
        fwrite(zeros, 1, pad, f);
    }
}

/* HMAC-SHA256 over seg1 then seg2 (manifest.json bytes, then the sole
 * bytecode entry - atelier.py's cmd_pack signs "manifest + bytecode
 * entries sorted by name", trivially just these two with one entry),
 * hex-encoded into out_hex[65]. Sign-side counterpart to
 * verify_hmac_segments() above; simpler because both inputs are already
 * whole buffers in RAM (the embedded arrays below), not a file streamed
 * in CHUNK_SIZE pieces - nothing here is untrusted network/serial input
 * the way an incoming install's package is. */
static bool sign_hmac(const uint8_t *seg1, size_t len1, const uint8_t *seg2, size_t len2,
                       const uint8_t *key, size_t key_len, char out_hex[65]) {
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS && st != PSA_ERROR_ALREADY_EXISTS) return false;

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);

    mbedtls_svc_key_id_t key_id;
    if (psa_import_key(&attrs, key, key_len, &key_id) != PSA_SUCCESS) return false;

    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    bool ok = psa_mac_sign_setup(&op, key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS;
    ok = ok && psa_mac_update(&op, seg1, len1) == PSA_SUCCESS;
    ok = ok && psa_mac_update(&op, seg2, len2) == PSA_SUCCESS;

    uint8_t mac[32];
    size_t mac_len = 0;
    if (ok) {
        ok = psa_mac_sign_finish(&op, mac, sizeof mac, &mac_len) == PSA_SUCCESS;
        if (!ok) psa_mac_abort(&op);
    } else {
        psa_mac_abort(&op);
    }
    psa_destroy_key(key_id);
    if (!ok || mac_len != sizeof mac) return false;

    for (size_t i = 0; i < sizeof mac; i++) snprintf(out_hex + i * 2, 3, "%02x", mac[i]);
    return true;
}

/* Default out-of-box face (docs/design/launcher-states.md §4, project
 * chat 2026-09-05: "the watch shows the time after power-on... without
 * anyone installing anything"). This is §4's actual recommended
 * mechanism, not the fixed-key shortcut an earlier pass of this function
 * used (2026-09-05, same day - replaced once the real per-device key was
 * confirmed working end to end): default_face_pkg.h embeds only the
 * *unsigned* manifest.json + bytecode; this signs them at runtime with
 * get_hmac_key()'s real per-device key, assembles a tar container, and
 * installs it through the exact same kb_store_install() path any real
 * package uses - no bypass, no fixed key anywhere in the repo.
 *
 * Skips if a watchface-type package with a *different* id already
 * exists (a real user's own face, never overwritten) - but not if it's
 * "kaliber.default" itself, so a device that already has the old fixed-
 * key-signed copy (installed before this function was rewritten) still
 * gets it replaced with a properly-signed one on its next boot, rather
 * than skipping forever because "a watchface already exists". Cheap and
 * safe to call unconditionally every boot either way: the embedded
 * source costs nothing to re-sign, and kb_store_install()'s own rename-
 * over-existing swap makes reinstalling under the same id a no-op in
 * effect once the signature already matches. */
void kb_store_install_default_face(void) {
    static const char *DEFAULT_FACE_ID = "kaliber.default";

    char ids[8][KB_APP_ID_MAX];
    int n = kb_store_list(ids, 8);
    for (int i = 0; i < n; i++) {
        kb_manifest_t mf;
        if (kb_store_read_manifest(ids[i], &mf) == ESP_OK && mf.type == KB_APP_WATCHFACE
            && strcmp(ids[i], DEFAULT_FACE_ID) != 0) {
            ESP_LOGI(TAG, "default face: skipped, '%s' already covers watchface", ids[i]);
            return;
        }
    }

    uint8_t key[32];
    if (get_hmac_key(key) != ESP_OK) {
        ESP_LOGE(TAG, "default face: could not obtain this device's HMAC key");
        return;
    }

    char sig_hex[65];
    if (!sign_hmac(k_default_face_manifest, sizeof k_default_face_manifest,
                    k_default_face_qjb, sizeof k_default_face_qjb,
                    key, sizeof key, sig_hex)) {
        ESP_LOGE(TAG, "default face: HMAC signing failed");
        return;
    }

    const char *pkg_path = ROOT "/.default_face.comp";
    FILE *pf = fopen(pkg_path, "wb");
    if (!pf) {
        ESP_LOGE(TAG, "default face: could not stage package");
        return;
    }
    write_tar_entry(pf, "manifest.json", k_default_face_manifest, sizeof k_default_face_manifest);
    write_tar_entry(pf, "app.qjb", k_default_face_qjb, sizeof k_default_face_qjb);
    write_tar_entry(pf, "sig.hmac", (const uint8_t *)sig_hex, 64); /* raw hex, no NUL - matches atelier.py's sig.hmac */
    uint8_t zeros[512] = {0};
    fwrite(zeros, 1, sizeof zeros, pf); /* end-of-archive marker, two all-zero blocks per POSIX tar */
    fwrite(zeros, 1, sizeof zeros, pf);
    fclose(pf);

    char id[KB_APP_ID_MAX];
    esp_err_t err = kb_store_install(pkg_path, id);
    unlink(pkg_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "default face: install failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "default face: installed '%s', signed with this device's own key", id);
}
