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
#include "gfx/text.h"
#include "cadran/face_format.h"

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

/* Renders every widget in `face` into ctx->fb (one stripe's buffer, or
 * the whole panel for a non-striped board - see gfx/text.h's gfx_ctx_t
 * and docs/design/display-regions.md). A widget whose bind_id resolves
 * to no value (provider not available) is skipped, not an error - see
 * design doc §3. A widget straddling ctx's stripe boundary is clipped,
 * not skipped - the same per-pixel bounds check that already clips at
 * the panel edge. */
esp_err_t cadran_render(const cadran_face_t *face, const gfx_ctx_t *ctx);

/* Bring-up self-test: builds a small hand-authored face in memory, runs
 * it through cadran_face_load() + cadran_render() into a scratch
 * framebuffer, and logs PASS/FAIL. Temporary - remove this and its call
 * site (main.c) once cadran_render() is wired into the launcher for real
 * (roadmap step 6) and can be exercised through an actual installed app
 * instead. */
void cadran_selftest(void);

/* Resolves one provider (time/date/battery - see face_format.h's
 * cadran_provider_id_t). Returns false if unavailable (unknown id, no
 * board hook, or a not-yet-implemented app.N slot) - a Cadran widget
 * skips itself on false (design doc §3); jw.sensors.Time()/Battery()
 * (unruh/modules/js_sensors.c) call this same function for the data
 * they don't already get elsewhere - one provider table, two callers,
 * per docs/design/js-api.md §4's Time section, not two implementations
 * of "read the clock" to keep in sync. Was internal-only
 * (cadran_internal.h) until js-api.md needed a second caller outside
 * this component. */
bool cadran_provider_get(cadran_provider_id_t id, const board_desc_t *board,
                          cadran_value_t *out);

#ifdef __cplusplus
}
#endif
