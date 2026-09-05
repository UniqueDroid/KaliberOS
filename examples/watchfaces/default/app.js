/* Default watchface — the out-of-box clock face (project chat
 * 2026-09-05: "the watch shows the time after power-on, big, without
 * anyone installing anything"). ES5 only (MQuickJS-compatible baseline).
 *
 * Declarative (WatchFace, not App) - build() runs once, at install time
 * and on manifest/ABI mismatch (docs/design/launcher-states.md §4,
 * cadran-watchface-engine.md §8); every minute tick after that renders
 * through cadran_render() in pure C, no engine involved.
 *
 * Position is relative to ctx.w/ctx.h, not hardcoded (found live,
 * 2026-09-05: this face's original x=20,y=84 was sized for watchy_v3's
 * 200x200 panel and rendered off-center on the C6's 410x502 one - the
 * fix is centering the same fixed layout on whatever panel build() gets
 * called with, not a second set of hardcoded numbers per board; that's
 * the whole point of build() receiving ctx at all, design doc §3).
 *
 * time.hm is always "HH:MM" (providers.c), 5 chars fixed-width at
 * scale 4 (32px/glyph = 160x32px total) - a fixed scale, not yet
 * relative to ctx.w/ctx.h itself (scale 6, examples/watchfaces/simple/
 * face.js's own value, would already overflow watchy_v3's 200px width
 * at 5*48=240px; scaling *up* for the C6's larger panel is a real next
 * step, just a separate one from centering).
 */
WatchFace({
  build: function (ctx) {
    var scale = 4;
    var w = 5 * 8 * scale;  /* "HH:MM", 8px/glyph before scaling */
    var h = 8 * scale;
    return {
      widgets: [
        {
          type: "text",
          x: Math.floor((ctx.w - w) / 2),
          y: Math.floor((ctx.h - h) / 2),
          bind: "time.hm", format: "{v}", scale: scale
        }
      ]
    };
  }
});
