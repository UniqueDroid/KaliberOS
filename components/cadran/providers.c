#include <time.h>
#include <string.h>
#include <stdio.h>
#include "cadran_internal.h"

bool cadran_provider_get(cadran_provider_id_t id, const board_desc_t *board,
                          cadran_value_t *out) {
    memset(out, 0, sizeof *out);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    switch (id) {
    case CADRAN_PROVIDER_TIME_H:
        out->i32 = tmv.tm_hour;
        return true;
    case CADRAN_PROVIDER_TIME_M:
        out->i32 = tmv.tm_min;
        return true;
    case CADRAN_PROVIDER_TIME_HM:
        out->is_string = true;
        snprintf(out->str, sizeof out->str, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        return true;
    case CADRAN_PROVIDER_TIME_MIN_ANGLE:
        out->i32 = tmv.tm_min * 6; /* 360deg / 60min, 0deg = 12 o'clock */
        return true;
    case CADRAN_PROVIDER_TIME_HOUR_ANGLE:
        out->i32 = (tmv.tm_hour % 12) * 30 + tmv.tm_min / 2; /* 360/12, plus minute creep */
        return true;
    case CADRAN_PROVIDER_DATE_D:
        out->i32 = tmv.tm_mday;
        return true;
    case CADRAN_PROVIDER_DATE_M:
        out->i32 = tmv.tm_mon + 1;
        return true;
    case CADRAN_PROVIDER_DATE_WD:
        out->i32 = tmv.tm_wday; /* 0=Sunday, matches struct tm */
        return true;
    case CADRAN_PROVIDER_BATTERY_PCT: {
        if (!board || !board->power || !board->power->battery_mv) return false;
        uint32_t mv = board->power->battery_mv();
        /* Linear map over a typical single-cell LiPo range. TODO: replace
         * with a real discharge curve once one exists - this is a rough
         * placeholder, not calibrated against any specific cell. */
        int pct = ((int)mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        out->i32 = pct;
        return true;
    }
    default:
        /* CADRAN_PROVIDER_NONE, unknown ids, and the app.0..7 slots (not
         * writable by anything yet - no hybrid-face JS API exists). */
        return false;
    }
}
