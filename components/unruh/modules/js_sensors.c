/**
 * jw.sensors — Time/Battery/Step, all pull (read-once-per-render fits
 * the tick model, docs/design/js-api.md §4). Registered only if the
 * manifest declares the "sensors" permission (launcher.c) - existence,
 * not a runtime check, per that doc's §3. Every factory below (Time()/
 * Battery()/Step(), no `new`) returns a fresh, stateless object - none
 * of these sensors have per-instance state, they all just read live
 * data at call time, same as Zepp's real classes behave from the
 * caller's point of view.
 *
 * Capability rule (§3, stated once here): a method whose normal return
 * type is number/boolean returns JS `null` when the board can't answer
 * - not `0`/`false`, which would read as a confirmed real value.
 *
 * onChange/offChange (Zepp's real Battery/Step classes both have these)
 * are deliberately not implemented yet - §5/§6's open point (push
 * callbacks are an App-only pattern; whether a Watchface's build()
 * should be denied them outright or just documented as a no-op isn't
 * settled) is left unresolved rather than guessed at here. Nothing in
 * the first wave's actual usage - a build()'s one-shot read, an App's
 * per-tick getCurrent() re-read - needs push delivery yet.
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "board_hal/board.h"
#include "unruh/engine.h"
#include "quickjs.h"
#include "cadran/cadran.h"

/* --------------------------------------------------------------- Time */

/* Direct time()/localtime_r(), not cadran_provider_get() - unlike
 * Battery below, Time needs full struct tm (year, seconds) plus an
 * epoch-ms value that cadran_provider_id_t has no entries for at all:
 * its enum only carries what a watchface widget actually binds to
 * (h/m/hm/angles/date), and no widget has ever needed a bare year or a
 * millisecond epoch. Always capable either way (no hardware dependency,
 * §4's Time section), so there's no gating logic worth sharing here -
 * just the same time()/localtime_r() call cadran/providers.c itself
 * makes. Battery below is the one that actually reuses the provider
 * table, per js-api.md §4's "one provider table, two callers" note. */

static JSValue time_get_time(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* JS number (double), not int32 - epoch ms overflows int32 long
     * before any real clock value, same as real Date.getTime(). */
    return JS_NewFloat64(ctx, (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0);
}

static void get_tm(struct tm *out) {
    time_t now = time(NULL);
    localtime_r(&now, out);
}

static JSValue time_get_full_year(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_year + 1900);
}
static JSValue time_get_month(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_mon + 1); /* Zepp: 1-12, not JS Date's 0-11 */
}
static JSValue time_get_date(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_mday);
}
static JSValue time_get_hours(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_hour);
}
static JSValue time_get_minutes(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_min);
}
static JSValue time_get_seconds(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    return JS_NewInt32(ctx, tmv.tm_sec);
}
static JSValue time_get_day(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct tm tmv; get_tm(&tmv);
    /* Zepp: 1-7, 1=Monday - not struct tm's 0=Sunday..6=Saturday. */
    int wd = (tmv.tm_wday == 0) ? 7 : tmv.tm_wday;
    return JS_NewInt32(ctx, wd);
}
static JSValue time_get_hour_format(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)ctx; (void)t; (void)argc; (void)argv;
    /* No 12h/24h user setting exists anywhere in this project yet -
     * a real (if unconditional) answer, not a guess. Both Zepp names
     * (v1.0 getFormatHour, v3+ getHourFormat, §4) point at the same
     * function here - not two behaviors to keep in sync. */
    return JS_NewInt32(ctx, 24);
}

static JSValue make_time_obj(JSContext *ctx) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "getTime", JS_NewCFunction(ctx, time_get_time, "getTime", 0));
    JS_SetPropertyStr(ctx, o, "getFullYear", JS_NewCFunction(ctx, time_get_full_year, "getFullYear", 0));
    JS_SetPropertyStr(ctx, o, "getMonth", JS_NewCFunction(ctx, time_get_month, "getMonth", 0));
    JS_SetPropertyStr(ctx, o, "getDate", JS_NewCFunction(ctx, time_get_date, "getDate", 0));
    JS_SetPropertyStr(ctx, o, "getHours", JS_NewCFunction(ctx, time_get_hours, "getHours", 0));
    JS_SetPropertyStr(ctx, o, "getMinutes", JS_NewCFunction(ctx, time_get_minutes, "getMinutes", 0));
    JS_SetPropertyStr(ctx, o, "getSeconds", JS_NewCFunction(ctx, time_get_seconds, "getSeconds", 0));
    JS_SetPropertyStr(ctx, o, "getDay", JS_NewCFunction(ctx, time_get_day, "getDay", 0));
    JS_SetPropertyStr(ctx, o, "getHourFormat", JS_NewCFunction(ctx, time_get_hour_format, "getHourFormat", 0));
    JS_SetPropertyStr(ctx, o, "getFormatHour", JS_NewCFunction(ctx, time_get_hour_format, "getFormatHour", 0));
    return o;
}

static JSValue sensors_time_factory(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return make_time_obj(ctx);
}

/* ------------------------------------------------------------ Battery */

static JSValue battery_get_current(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    cadran_value_t v;
    /* Reuses cadran's mV->% linear-map (providers.c) instead of
     * duplicating it - the one module in this file where that reuse
     * actually applies, see the Time section's comment on why it
     * doesn't apply there too. */
    if (!cadran_provider_get(CADRAN_PROVIDER_BATTERY_PCT, board_get(), &v)) return JS_NULL;
    return JS_NewInt32(ctx, v.i32);
}

static JSValue battery_is_charging(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const board_desc_t *b = board_get();
    /* null, not false - §3's rule: "confirmed not charging" and "can't
     * tell" are different facts, and false reads as the former. */
    if (!b->power->charging) return JS_NULL;
    return JS_NewBool(ctx, b->power->charging());
}

static JSValue sensors_battery_factory(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "getCurrent", JS_NewCFunction(ctx, battery_get_current, "getCurrent", 0));
    JS_SetPropertyStr(ctx, o, "isCharging", JS_NewCFunction(ctx, battery_is_charging, "isCharging", 0));
    return o;
}

/* --------------------------------------------------------------- Step */

static JSValue step_get_current(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const board_desc_t *b = board_get();
    if (!b->sensors->step_count) return JS_NULL;
    return JS_NewInt32(ctx, b->sensors->step_count());
}

static JSValue step_get_target(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const board_desc_t *b = board_get();
    if (!b->sensors->step_target) return JS_NULL;
    return JS_NewInt32(ctx, b->sensors->step_target());
}

static JSValue sensors_step_factory(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "getCurrent", JS_NewCFunction(ctx, step_get_current, "getCurrent", 0));
    JS_SetPropertyStr(ctx, o, "getTarget", JS_NewCFunction(ctx, step_get_target, "getTarget", 0));
    return o;
}

/* ------------------------------------------------------------- install */

static int install(js_engine_t *e, void *ctx_opaque) {
    (void)e;
    JSContext *ctx = ctx_opaque;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue jw = JS_GetPropertyStr(ctx, g, "jw");
    JSValue sensors = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, sensors, "Time", JS_NewCFunction(ctx, sensors_time_factory, "Time", 0));
    JS_SetPropertyStr(ctx, sensors, "Battery", JS_NewCFunction(ctx, sensors_battery_factory, "Battery", 0));
    JS_SetPropertyStr(ctx, sensors, "Step", JS_NewCFunction(ctx, sensors_step_factory, "Step", 0));

    JS_SetPropertyStr(ctx, jw, "sensors", sensors);
    JS_FreeValue(ctx, jw);
    JS_FreeValue(ctx, g);
    return 0;
}

const js_module_def_t jw_sensors_module = { .name = "jw_sensors", .install = install };
