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
 * Layout centered relative to ctx.w/ctx.h (not hardcoded per board, same
 * reasoning as examples/watchfaces/default/app.js's own header comment)
 * - width estimates per line are fixed-width guesses (5 chars "HH:MM",
 * 4 chars "100%", 11 chars "12345 steps"), not measured from the actual
 * live value, so centering is approximate for the two shorter lines,
 * same trade-off the default face already accepts for time.hm.
 */
WatchFace({
  build: function (ctx) {
    var timeScale = 4, lineScale = 2;
    var timeW = 5 * 8 * timeScale, timeH = 8 * timeScale;
    var battW = 4 * 8 * lineScale;
    var stepW = 11 * 8 * lineScale;
    var lineH = 8 * lineScale;
    var gap = 8;

    var blockH = timeH + gap + lineH + gap + lineH;
    var top = Math.floor((ctx.h - blockH) / 2);

    return {
      widgets: [
        {
          type: "text",
          x: Math.floor((ctx.w - timeW) / 2), y: top,
          bind: "time.hm", format: "{v}", scale: timeScale
        },
        {
          type: "text",
          x: Math.floor((ctx.w - battW) / 2), y: top + timeH + gap,
          bind: "battery.pct", format: "{v}%", scale: lineScale
        },
        {
          type: "text",
          x: Math.floor((ctx.w - stepW) / 2), y: top + timeH + gap + lineH + gap,
          bind: "steps.count", format: "{v} steps", scale: lineScale
        }
      ]
    };
  }
});
