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
 * font: "large" (project chat 2026-09-30, design doc §5a) - a role, not
 * a pixel size or upscale factor: render.c resolves it per board/tick
 * to the biggest registered font that both covers every character in
 * "HH:MM" and fits this panel's own width, falling back toward "small"
 * if the ideal size doesn't fit (as it won't on watchy_v3's 200px panel
 * - the same face.bin renders correctly smaller there, no per-board
 * branch here). charW below is only a *centering estimate*, not the
 * real glyph width (that's resolved in C, after this runs) - scaled by
 * ctx.w so it tracks whichever font C is actually going to pick closely
 * enough (roughly 64px/char on the C6, 32px/char on watchy_v3, both
 * real registered sizes) without build() needing to duplicate render.c's
 * own fallback logic. A few pixels of centering error either way is the
 * accepted trade-off already documented on the dashboard face.
 */
WatchFace({
  build: function (ctx) {
    var charW = Math.floor(ctx.w / 6);
    var w = 5 * charW;             /* "HH:MM" */
    var h = Math.floor(charW * 1.5); /* rough cell aspect, centering only */
    return {
      widgets: [
        {
          type: "text",
          x: Math.floor((ctx.w - w) / 2),
          y: Math.floor((ctx.h - h) / 2),
          bind: "time.hm", format: "{v}", font: "large"
        }
      ]
    };
  }
});
