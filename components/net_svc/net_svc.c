/**
 * See net_svc.h for the overall design (on-demand only, torn down before
 * returning, streamed uploads).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "board_hal/board.h"
#include "gfx/text.h"
#include "core/app_store.h"
#include "net_svc/net_svc.h"

static const char *TAG = "net_svc";

#define STAGING_DIR    "/apps/.staging"
#define UPLOAD_PATH    STAGING_DIR "/upload.comp"
#define HTTP_CHUNK     512

/* ---------------------------------------------------------- AP identity */

/* Per-device SSID (no two boards on the bench collide), per-device
 * password (Kconfig override first, else NVS - generated+logged once,
 * same pairing-code idea as app_store's HMAC key, never a fixed value in
 * source control). */
static void ap_ssid(char out[33]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, 33, "KaliberOS-%02X%02X", mac[4], mac[5]);
}

#define NVS_NAMESPACE "kaliber_net"
#define NVS_KEY_PASS  "ap_pass"

static bool ap_password(char out[65]) {
    if (strlen(CONFIG_KALIBER_NET_AP_PASSWORD) >= 8) {
        strlcpy(out, CONFIG_KALIBER_NET_AP_PASSWORD, 65);
        return true;
    }
    if (CONFIG_KALIBER_NET_AP_PASSWORD[0] != '\0') {
        ESP_LOGW(TAG, "KALIBER_NET_AP_PASSWORD set but shorter than WPA2's 8-char "
                       "minimum - ignoring it, falling back to NVS/open");
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = 65;
        if (nvs_get_str(h, NVS_KEY_PASS, out, &len) == ESP_OK && strlen(out) >= 8) {
            nvs_close(h);
            return true;
        }
        static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"; /* no 0/O/1/I */
        char gen[9];
        for (int i = 0; i < 8; i++) gen[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
        gen[8] = '\0';
        if (nvs_set_str(h, NVS_KEY_PASS, gen) == ESP_OK && nvs_commit(h) == ESP_OK) {
            ESP_LOGW(TAG, "generated new sync-mode AP password: %s", gen);
            strlcpy(out, gen, 65);
            nvs_close(h);
            return true;
        }
        nvs_close(h);
    }
    ESP_LOGE(TAG, "could not obtain/generate an AP password - falling back to OPEN (no password)");
    return false;
}

/* ------------------------------------------------------------- display */

/* Direct-framebuffer status screen, no JS engine/Cadran involved - same
 * "C path" idea as cadran_selftest(). Heap-allocated (board_fb_size() is
 * a few KB, well past what belongs on any task stack in this project -
 * see README's "big buffers on the heap" convention). */
static void draw_sync_screen(const char *ssid, const char *pass, const char *ip) {
    const board_desc_t *b = board_get();
    uint8_t *fb = malloc(board_fb_size());
    if (!fb) return;
    char line[80]; /* generous: GCC's format-truncation check assumes pass[]
                     * (declared 65) could be fully used, "PASS: " + 64 + NUL
                     * already exceeds a tighter buffer here. */

    /* Fixed content at panel-absolute coordinates, drawn fresh per
     * stripe (docs/design/display-regions.md) - stripe=disp_h on
     * Watchy means this loop runs once, same as before stripes
     * existed. */
    uint16_t stripe = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    if (b->display->begin_frame) b->display->begin_frame();
    for (int y = 0; y < b->caps.disp_h; y += stripe) {
        int h = stripe;
        if (y + h > b->caps.disp_h) h = b->caps.disp_h - y;
        gfx_ctx_t ctx = { .fb = fb, .board = b, .origin_y = y, .height = h };
        memset(fb, 0xFF, board_fb_size()); /* white, matches jw_ui clear() */

        gfx_draw_text(&ctx, 10, 10, "SYNC MODE", 2);
        snprintf(line, sizeof line, "SSID: %s", ssid);
        gfx_draw_text(&ctx, 10, 50, line, 1);
        snprintf(line, sizeof line, "PASS: %s", pass);
        gfx_draw_text(&ctx, 10, 65, line, 1);
        snprintf(line, sizeof line, "IP:   %s:8080", ip);
        gfx_draw_text(&ctx, 10, 80, line, 1);
        gfx_draw_text(&ctx, 10, 110, "POST /install", 1);

        b->display->blit_region(0, y, b->caps.disp_w, h, fb);
    }
    b->display->end_frame(true);
    free(fb);
}

/* ------------------------------------------------------------- httpd */

static volatile int64_t s_last_activity_us;

static void touch_activity(void) { s_last_activity_us = esp_timer_get_time(); }

static esp_err_t install_post_handler(httpd_req_t *req) {
    touch_activity();
    mkdir(STAGING_DIR, 0755); /* ok if it already exists */

    FILE *f = fopen(UPLOAD_PATH, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "staging open failed");
        return ESP_FAIL;
    }

    /* Streamed straight to flash, HTTP_CHUNK bytes at a time - never the
     * whole upload in RAM at once (see net_svc.h / app_store.c's module
     * comment: a real .comp can run to hundreds of KB, more than
     * comfortably fits alongside WiFi's own resident heap cost). Heap
     * logged here, at the very start of the stream, so it's directly
     * comparable against the same handler's own free-heap log right
     * after kb_store_install() below: if streaming ever regressed into
     * buffering the whole body first, content_len-sized bytes of that
     * gap would show up between these two numbers, not just at the end. */
    ESP_LOGI(TAG, "install: upload starting, %d B declared, free heap %u B",
             req->content_len, (unsigned)esp_get_free_heap_size());
    char buf[HTTP_CHUNK];
    int remaining = req->content_len;
    bool ok = true;
    while (ok && remaining > 0) {
        int want = remaining < (int)sizeof buf ? remaining : (int)sizeof buf;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        ok = fwrite(buf, 1, r, f) == (size_t)r;
        remaining -= r;
    }
    fclose(f);
    touch_activity();

    if (!ok) {
        unlink(UPLOAD_PATH);
        ESP_LOGE(TAG, "install: upload stream failed (%d B remaining)", remaining);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "install: upload done, free heap %u B (see the 'starting' line above)",
             (unsigned)esp_get_free_heap_size());
    char id[KB_APP_ID_MAX] = {0};
    esp_err_t err = kb_store_install(UPLOAD_PATH, id);
    unlink(UPLOAD_PATH);

    char resp[128];
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":\"%s\"}", id);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(resp, sizeof resp, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static const httpd_uri_t install_uri = {
    .uri = "/install", .method = HTTP_POST, .handler = install_post_handler,
};

/* ------------------------------------------------------------- run loop */

void kb_net_svc_run_sync_mode(void) {
    /* Kept and re-logged together at exit (below), not just here - the
     * "entering"/"AP up" lines land in the first ~1-2s after a USB reset,
     * a window this project's serial capture setup has never reliably
     * caught (project chat, all session); the exit-time summary always
     * has (see e.g. 2026-09-04's own timeout-teardown line). */
    unsigned heap_entering = (unsigned)esp_get_free_heap_size();
    ESP_LOGI(TAG, "sync mode: entering, free heap %u B", heap_entering);

    /* Idempotent: only the very first sync-mode call in a boot needs
     * these, later calls (a later wake, still plugged in) find them
     * already done. esp_netif/esp_event are one-shot-per-boot by design,
     * unlike esp_wifi_init()/deinit() below which this function pairs
     * itself every single call. */
    static bool s_net_stack_ready;
    if (!s_net_stack_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_net_stack_ready = true;
    }
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    char ssid[33], pass[65];
    ap_ssid(ssid);
    bool have_pass = ap_password(pass);

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof ap_cfg.ap.ssid);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 2;
    if (have_pass) {
        strlcpy((char *)ap_cfg.ap.password, pass, sizeof ap_cfg.ap.password);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        strlcpy(pass, "(none - open network)", sizeof pass);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_get_ip_info(ap_netif, &ip_info);
    char ip_str[16];
    snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&ip_info.ip));

    unsigned heap_ap_up = (unsigned)esp_get_free_heap_size();
    ESP_LOGI(TAG, "sync mode: AP '%s' up, pass=%s, ip=%s, free heap %u B",
             ssid, pass, ip_str, heap_ap_up);
    draw_sync_screen(ssid, pass, ip_str);

    httpd_handle_t httpd = NULL;
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    /* kb_store_install() itself now works off small fixed-size chunks
     * and heap allocations, not a stack array (see the 2026-09-04
     * mkdir()/stack-overflow postmortem) - its own measured high-water
     * mark (logged on every install) should be well under this once a
     * real sync-mode install has run at least once; sized generously
     * ahead of that measurement rather than at ESP-IDF's ~4 kB default,
     * which was tight even for the *old*, buggier code. Revisit once
     * there's a real number from the field. */
    httpd_cfg.stack_size = 8192;
    /* Matches tools/atelier/atelier.py's cmd_push() default (--port
     * 8080) - no reason for this project's own tool and firmware to
     * disagree on a default that's otherwise invisible until someone
     * hits exactly this mismatch. */
    httpd_cfg.server_port = 8080;
    if (httpd_start(&httpd, &httpd_cfg) == ESP_OK) {
        httpd_register_uri_handler(httpd, &install_uri);
    } else {
        ESP_LOGE(TAG, "sync mode: httpd_start() failed - AP is up but nothing is listening");
    }

    touch_activity();
    int64_t timeout_us = (int64_t)CONFIG_KALIBER_NET_SYNC_TIMEOUT_S * 1000000;
    while (esp_timer_get_time() - s_last_activity_us < timeout_us) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "sync mode: idle timeout, tearing down");

    if (httpd) httpd_stop(httpd);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy(ap_netif);

    unsigned heap_exit = (unsigned)esp_get_free_heap_size();
    ESP_LOGI(TAG, "sync mode: exited, free heap %u B", heap_exit);
    /* The number that actually answers "did esp_wifi_deinit() give
     * everything back" - entering vs exiting, side by side, in the part
     * of the log that reliably survives capture (see the comment at the
     * top of this function). A gap here across repeated sync-mode cycles
     * (not just one) would be the leak to worry about - a watch left
     * plugged in runs this every wake. */
    ESP_LOGI(TAG, "sync mode summary: entering=%u ap_up=%u (wifi cost %d) exiting=%u (net %+d vs entering)",
             heap_entering, heap_ap_up, (int)heap_entering - (int)heap_ap_up,
             heap_exit, (int)heap_exit - (int)heap_entering);
}
