/**
 * Cadran - the Kaliber watchface engine (design doc:
 * docs/design/cadran-watchface-engine.md). Renders a face.bin widget tree
 * without booting the JS engine.
 *
 * Roadmap status (2026-09-01): steps 1-3 only (loader, rect/line/hand
 * widget renderers, provider registry). No font rasterizer or atelier
 * resource pipeline yet, so text/img/img_digits/arc/img_level widgets are
 * parsed but skipped at render time, not implemented. Not wired into the
 * launcher (roadmap step 6) - a declarative face's minute ticks still go
 * through Unruh today.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "board_hal/board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cadran_face cadran_face_t;

/* Parses + validates a face.bin buffer and copies it into a new owned
 * allocation (the caller's buffer can be freed/reused right after this
 * returns, success or not). */
esp_err_t cadran_face_load(const uint8_t *data, size_t len, cadran_face_t **out_face);
void      cadran_face_free(cadran_face_t *face);
uint8_t   cadran_face_widget_count(const cadran_face_t *face);

/* Renders every widget in `face` into `fb` (a canonical framebuffer sized
 * board_fb_size() bytes, in the format board->caps.disp_kind implies).
 * A widget whose bind_id resolves to no value (provider not available)
 * is skipped, not an error - see design doc §3. */
esp_err_t cadran_render(const cadran_face_t *face, uint8_t *fb, const board_desc_t *board);

/* Bring-up self-test: builds a small hand-authored face in memory, runs
 * it through cadran_face_load() + cadran_render() into a scratch
 * framebuffer, and logs PASS/FAIL. Temporary - remove this and its call
 * site (main.c) once cadran_render() is wired into the launcher for real
 * (roadmap step 6) and can be exercised through an actual installed app
 * instead. */
void cadran_selftest(void);

#ifdef __cplusplus
}
#endif
