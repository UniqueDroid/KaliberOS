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
    jw.ui.text(20, 60, "Kaliber");
    jw.ui.text(20, 100, "wakes: " + this.count);
  },
  onSuspend: function () {
    return { count: this.count };
  }
});
