/* Simple declarative watchface — ES5 only (MQuickJS-compatible baseline).
 *
 * build() runs once (see docs/design/cadran-watchface-engine.md §1); the
 * returned tree is serialized to face.bin and every RTC-tick render after
 * that goes through cadran_render() in pure C, no JS engine involved.
 * First real target for the design's core promise, per the roadmap step
 * 4 discussion - not wired into the launcher yet (step 6), currently only
 * exercised by unruh's js_watchface_selftest().
 */
WatchFace({
  build: function (ctx) {
    return {
      widgets: [
        { type: "text", x: 20, y: 80, bind: "time.hm", format: "{v}", scale: 6 }
      ]
    };
  }
});
