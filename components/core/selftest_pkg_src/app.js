/* Hello complication — ES5 only (MQuickJS-compatible baseline).
 * Copy of examples/complications/hello/app.js, packed under this
 * directory's own manifest.json (reserved id ".selftest.hello", not the
 * real "de.jan.hello") for store_install_selftest_pkg.h's embedded
 * package - see app_store.c's kb_store_install_selftest() comment. Keep
 * in sync with the real example by hand; the two are allowed to diverge,
 * this one only needs to compile and round-trip through the store. */
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
