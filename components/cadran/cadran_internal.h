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

/* cadran_provider_get() itself is declared in the public cadran.h now -
 * js-api.md's jw.sensors needs it too, see that declaration's comment. */

#ifdef __cplusplus
}
#endif
