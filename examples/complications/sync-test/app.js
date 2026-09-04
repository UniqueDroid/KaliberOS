/* sync-test complication — proves a real net_svc.c push landed, not the
 * bring-up copy seed_hello_app() writes straight to /apps/hello (a
 * different id, "hello" vs this one's "de.jan.sync-test" - see
 * main/seed_apps.c). Deliberately different on-screen text from hello's
 * "KALIBER"/"WAKES:" so a glance at the panel settles it either way. */
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
    jw.ui.text(20, 20, "SYNCED!", 2);
    jw.ui.text(20, 50, "via WiFi", 1);
    var shown = this.count % 100;
    var digits = (shown < 10 ? "0" : "") + shown;
    jw.ui.text(52, 80, digits, 6);
  },
  onSuspend: function () {
    return { count: this.count };
  }
});
