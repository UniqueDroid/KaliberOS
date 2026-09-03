/* Hello complication — ES5 only (MQuickJS-compatible baseline). */
App({
  onInit: function () {
    this.count = 0;
  },
  onResume: function (state) {
    this.count = state.count || 0;
  },
  onEvent: function (ev) {
    if (ev.type === "button") {
      this.count += 1;
      jw.ui.invalidate();
    }
  },
  onRender: function () {
    jw.ui.clear();
    jw.ui.text(20, 20, "KALIBER", 2);
    jw.ui.text(20, 50, "WAKES:", 1);
    /* Full count is kept in state (onSuspend below) and never wraps; only
     * the on-screen digits are last-two, at a size actually readable on
     * the 200x200 panel without USB - see criterion-4 retest. */
    var shown = this.count % 100;
    var digits = (shown < 10 ? "0" : "") + shown;
    jw.ui.text(52, 80, digits, 6);
  },
  onSuspend: function () {
    return { count: this.count };
  }
});
