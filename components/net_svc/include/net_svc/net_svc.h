/**
 * Kaliber net_svc — on-demand WiFi sync endpoint.
 *
 * Not a background service: WiFi's own stack costs 50-70 kB of heap, more
 * than this project's entire JS budget (README's "Success criteria for
 * the first milestone" #1), so it only ever exists for the duration of
 * one call to kb_net_svc_run_sync_mode() - a self-contained SoftAP +
 * POST /install HTTP endpoint, torn back down (esp_wifi_stop()+
 * esp_wifi_deinit()) before returning no matter how the call ends.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Blocks the calling task for up to KALIBER_NET_SYNC_TIMEOUT_S seconds of
 * inactivity (Kconfig), running a SoftAP + HTTP server that accepts
 * `POST /install` (raw .comp body, streamed straight to
 * kb_store_install() - never buffered whole in RAM) and installs it via
 * kb_store_install(). Draws a status screen (SSID/password/IP) directly
 * onto the panel for the duration via gfx_draw_text(), the same
 * no-JS-engine "C path" cadran_selftest() uses - the caller's normal
 * render path (kb_launcher_start()) resumes once this returns.
 *
 * Always returns - never leaves the radio or the endpoint running past
 * this call, whether it ends via timeout or an explicit stop. Safe to
 * call again on a later wake. */
void kb_net_svc_run_sync_mode(void);

#ifdef __cplusplus
}
#endif
