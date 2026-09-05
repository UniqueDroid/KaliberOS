/* Default watchface — the out-of-box clock face (project chat
 * 2026-09-05: "the watch shows the time after power-on, big, without
 * anyone installing anything"). ES5 only (MQuickJS-compatible baseline).
 *
 * Declarative (WatchFace, not App) - build() runs once, at install time
 * and on manifest/ABI mismatch (docs/design/launcher-states.md §4,
 * cadran-watchface-engine.md §8); every minute tick after that renders
 * through cadran_render() in pure C, no engine involved.
 *
 * time.hm is always "HH:MM" (providers.c), 5 chars fixed-width - scale 4
 * (32px/glyph) is the largest that fits this panel's 200px width with
 * the app.js/native-screens.md hierarchy convention's margin (5*32=160,
 * x=20 leaves 20px each side); scale 6, examples/watchfaces/simple/
 * face.js's own value, overflows here (5*48=240).
 */
WatchFace({
  build: function (ctx) {
    return {
      widgets: [
        { type: "text", x: 20, y: 84, bind: "time.hm", format: "{v}", scale: 4 }
      ]
    };
  }
});
