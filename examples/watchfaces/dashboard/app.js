/* Dashboard watchface - the js-api.md acceptance test (Jan's go-ahead,
 * project chat 2026-09-05): "a face that shows time, battery and steps
 * and runs on both boards - with steps on one, without on the other, if
 * the sensor isn't wired up yet." ES5 only (MQuickJS-compatible baseline).
 *
 * Declarative (WatchFace, not App) - all three lines are bound providers
 * (time.hm/battery.pct/steps.count), read live every tick in pure C via
 * cadran_provider_get(), not read once in build(). This is deliberately
 * NOT the same path as jw.sensors.Step() (unruh/modules/js_sensors.c,
 * an imperative App's data source) - see js-api.md §4a and
 * cadran-watchface-engine.md §5's "one source, two facades" rule: both
 * facades read board_desc_t.sensors, but a Watchface's live numbers
 * come from the C-side provider, never from a jw.sensors call baked
 * into this build() (that would only ever show a frozen value).
 *
 * steps.count has no sensor_ops.step_count wired up on either board yet
 * (no IMU driver in this tree) - render.c's provider-unavailable rule
 * (design doc §3: skip the widget, not an error) means that line simply
 * doesn't draw on either board today, not a bug in this face. The
 * moment a board gets a real IMU driver, this same face starts showing
 * it with no face change - that's the acceptance test's actual point.
 *
 * font: "large"/"medium" (project chat 2026-09-30, design doc §5a) - a
 * role, not a pixel size; render.c resolves each to the biggest font
 * that covers the actual string and fits this board's panel, falling
 * back toward "small" where it doesn't (steps.count's "12345 steps" is
 * long enough that "medium" won't fit watchy_v3's 200px panel - it
 * falls back to "small" there, same face.bin, no per-board branch
 * here). Layout centered relative to ctx.w/ctx.h (not hardcoded per
 * board, same reasoning as examples/watchfaces/default/app.js's own
 * header comment) - charW below are *centering estimates* per role
 * (roughly what each role's primary registered font actually measures),
 * not measured from the live value or from whichever font C ends up
 * resolving to, so centering is approximate, same trade-off the default
 * face already accepts for its own time line.
 */
WatchFace({
  build: function (ctx) {
    var timeCharW = Math.floor(ctx.w / 6), timeH = Math.floor(timeCharW * 1.5);
    var lineCharW = 32, lineH = 32; /* "medium" role's primary font, gfx_font_32 */
    var timeW = 5 * timeCharW;         /* "HH:MM" */
    var battW = 4 * lineCharW;         /* "100%" */
    var stepW = 11 * lineCharW;        /* "12345 steps" */
    var gap = 8;

    var blockH = timeH + gap + lineH + gap + lineH;
    var top = Math.floor((ctx.h - blockH) / 2);

    return {
      widgets: [
        {
          type: "text",
          x: Math.floor((ctx.w - timeW) / 2), y: top,
          bind: "time.hm", format: "{v}", font: "large"
        },
        {
          type: "text",
          x: Math.floor((ctx.w - battW) / 2), y: top + timeH + gap,
          bind: "battery.pct", format: "{v}%", font: "medium"
        },
        {
          type: "text",
          x: Math.floor((ctx.w - stepW) / 2), y: top + timeH + gap + lineH + gap,
          bind: "steps.count", format: "{v} steps", font: "medium"
        }
      ]
    };
  }
});
