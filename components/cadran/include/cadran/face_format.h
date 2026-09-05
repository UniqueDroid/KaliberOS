/**
 * face.bin binary layout - see docs/design/cadran-watchface-engine.md §6.
 *
 * Little-endian. Three sections back to back: header, string table,
 * widget table. There is no explicit string-table-length field: the
 * loader derives it from the total buffer length minus the header and
 * the (fixed-size, widget_count-many) widget records, since the widget
 * table is always the last section. Keep it that way - don't add a
 * length field without also updating cadran_face_load()'s math.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CADRAN_ABI 1
#define CADRAN_STR_NONE 0xFFFFu

typedef struct __attribute__((packed)) {
    char     magic[4];   /* "CDRN" */
    uint8_t  abi;         /* CADRAN_ABI */
    uint8_t  widget_count;
    uint16_t flags;       /* reserved, 0 for now */
} cadran_header_t;

_Static_assert(sizeof(cadran_header_t) == 8, "cadran header must be 8 bytes");

/*
 * Fixed 24-byte widget record. `params[4]` is interpreted per widget
 * type - see the table below. `str_ref` is a byte offset into the string
 * table (CADRAN_STR_NONE = no string), used for image/resource names and
 * text format strings.
 *
 *   type          params[0]   params[1]   params[2]   params[3]
 *   RECT          w           h           filled(0/1) -
 *   LINE          x2          y2          -           -
 *   HAND          len         -           -           -
 *   TEXT/IMG/...  (reserved - not interpreted by this build yet)
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;      /* cadran_widget_type_t */
    uint8_t  bind_id;   /* cadran_provider_id_t, CADRAN_PROVIDER_NONE = unbound */
    int16_t  x, y;
    int16_t  params[4];
    uint16_t str_ref;
    uint8_t  reserved[8];
} cadran_widget_rec_t;

_Static_assert(sizeof(cadran_widget_rec_t) == 24, "cadran widget record must be 24 bytes");

typedef enum {
    CADRAN_WIDGET_IMG        = 0,
    CADRAN_WIDGET_IMG_DIGITS = 1,
    CADRAN_WIDGET_TEXT       = 2,
    CADRAN_WIDGET_ARC        = 3,
    CADRAN_WIDGET_IMG_LEVEL  = 4,
    CADRAN_WIDGET_HAND       = 5,
    CADRAN_WIDGET_RECT       = 6,
    CADRAN_WIDGET_LINE       = 7,
} cadran_widget_type_t;

/* "Always available" providers per design doc §5, plus the caps-gated
 * step pair (STEP_COUNT/STEP_TARGET) - hr/stress remain reserved,
 * not implemented by cadran_provider_get() yet. app.0..7 (hybrid-face-
 * writable slots) is a separate reserved range - see providers.c.
 *
 * STEP_COUNT/STEP_TARGET read board_desc_t.sensors the same way
 * jw.sensors.Step() does (unruh/modules/js_sensors.c) - js-api.md §4a's
 * "one source, not two" rule: a provider here must call the identical
 * sensor_ops_t field the JS module calls, never its own reader. */
typedef enum {
    CADRAN_PROVIDER_NONE            = 0,
    CADRAN_PROVIDER_TIME_H          = 1,
    CADRAN_PROVIDER_TIME_M          = 2,
    CADRAN_PROVIDER_TIME_HM         = 3,
    CADRAN_PROVIDER_TIME_MIN_ANGLE  = 4,
    CADRAN_PROVIDER_TIME_HOUR_ANGLE = 5,
    CADRAN_PROVIDER_DATE_D          = 6,
    CADRAN_PROVIDER_DATE_M          = 7,
    CADRAN_PROVIDER_DATE_WD         = 8,
    CADRAN_PROVIDER_BATTERY_PCT     = 9,
    CADRAN_PROVIDER_STEP_COUNT      = 10,
    CADRAN_PROVIDER_STEP_TARGET     = 11,
    CADRAN_PROVIDER_APP_0           = 16, /* .. CADRAN_PROVIDER_APP_0 + 7 */
} cadran_provider_id_t;

typedef struct {
    bool    is_string;
    int32_t i32;
    char    str[16];
} cadran_value_t;

#ifdef __cplusplus
}
#endif
