/**
 * Internal accessors shared between loader.c, render.c and providers.c.
 * Not part of the public component API (not under include/).
 */
#pragma once

#include "cadran/cadran.h"
#include "cadran/face_format.h"

#ifdef __cplusplus
extern "C" {
#endif

const cadran_widget_rec_t *cadran_face_widgets(const cadran_face_t *face);
const char *cadran_face_string(const cadran_face_t *face, uint16_t str_ref);

/* Resolves one provider. Returns false if unavailable (unknown id, no
 * board hook, or a not-yet-implemented app.N slot) - the caller then
 * skips the widget per the design doc's degradation rule. */
bool cadran_provider_get(cadran_provider_id_t id, const board_desc_t *board,
                          cadran_value_t *out);

#ifdef __cplusplus
}
#endif
