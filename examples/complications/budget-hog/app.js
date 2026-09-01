/* Budget-hog — deliberately hostile complication for testing the engine's
 * per-hook wall-clock budget (js_limits_t.hook_budget_ms, engine_quickjs.c's
 * interrupt_handler). onRender() never returns on its own; if the budget
 * mechanism works, the launcher sees a JS_ERR_TIMEOUT from js_call_hook()
 * (not a watchdog reset) and app_fail() logs it cleanly instead of the
 * device hanging or rebooting. */
App({
  onInit: function () {},
  onRender: function () {
    while (true) {
      /* spin */
    }
  },
});
