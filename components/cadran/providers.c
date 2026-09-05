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

    /* watchy_v3 has no RTC chip (project chat 2026-09-05) - time() is
     * newlib's, backed by esp_timer's counter, which starts at the Unix
     * epoch on power-on and is never set to real wall-clock time by
     * anything in this tree yet (no SNTP, no manual entry UI). tm_year
     * is years-since-1900; real synced time is never plausibly before
     * 2000 (tm_year 100), so this is a cheap, standard embedded-systems
     * way to tell "never synced" from "actually midnight" without a
     * dedicated has-been-set flag. time.hm degrades to a visible "??:??"
     * placeholder rather than a confidently wrong "00:0X" - the design
     * doc §3 "unavailable provider" rule (widget skipped, not shown at
     * all) fits a sensor that's genuinely absent, not a clock that's
     * merely unset yet; a face binding time.hm wants *something* on
     * screen either way. time.h/time.m and the hand-angle providers
     * still report unavailable (false) instead - a rotated-to-nonsense
     * clock hand is worse than no hand, unlike a text placeholder. */
    bool time_set = tmv.tm_year >= 100;

    switch (id) {
    case CADRAN_PROVIDER_TIME_H:
        if (!time_set) return false;
        out->i32 = tmv.tm_hour;
        return true;
    case CADRAN_PROVIDER_TIME_M:
        if (!time_set) return false;
        out->i32 = tmv.tm_min;
        return true;
    case CADRAN_PROVIDER_TIME_HM:
        out->is_string = true;
        /* "?" not "-" (found on hardware, 2026-09-05): gfx_font8x8.h's
         * 0x2d ('-') glyph is all-zero, an intentionally blank cell, not
         * a missing one - a "--:--" placeholder rendered as just a lone
         * ":" with nothing either side of it, exactly like real hardware
         * would; '?' (0x3f) has real glyph data. */
        if (time_set) snprintf(out->str, sizeof out->str, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        else          strlcpy(out->str, "??:??", sizeof out->str);
        return true;
    case CADRAN_PROVIDER_TIME_MIN_ANGLE:
        if (!time_set) return false;
        out->i32 = tmv.tm_min * 6; /* 360deg / 60min, 0deg = 12 o'clock */
        return true;
    case CADRAN_PROVIDER_TIME_HOUR_ANGLE:
        if (!time_set) return false;
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
    case CADRAN_PROVIDER_STEP_COUNT:
        /* Same board_desc_t.sensors field jw.sensors.Step().getCurrent()
         * reads (unruh/modules/js_sensors.c) - js-api.md §4a's "one
         * source, not two" rule, not a second reader of the IMU. */
        if (!board || !board->sensors || !board->sensors->step_count) return false;
        out->i32 = board->sensors->step_count();
        return true;
    case CADRAN_PROVIDER_STEP_TARGET:
        if (!board || !board->sensors || !board->sensors->step_target) return false;
        out->i32 = board->sensors->step_target();
        return true;
    default:
        /* CADRAN_PROVIDER_NONE, unknown ids, and the app.0..7 slots (not
         * writable by anything yet - no hybrid-face JS API exists). */
        return false;
    }
}
