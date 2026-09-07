# Base system — the out-of-box promise

Phase 0 of the current roadmap (project chat 2026-09-07): what a Kaliber
device can do *before anyone installs an app*, described as one whole,
measured against, not redesigned here. Everything below is already
decided somewhere - `launcher-states.md`, `js-api.md`,
`display-regions.md`, `package-signing.md`, `native-screens.md` - this
doc gathers it into one reference instead of leaving it scattered across
five docs written on five different days for five different reasons.

The promise, in one sentence: **a Kaliber device boots, shows a
watchface, has a menu you can always get back out of, can select and
start watchfaces and apps, can receive packages, and sleeps.** Six
clauses, six sections below - each states what stands, what's missing,
and what's board-dependent.

## 1. Boots

**Stands.** Cold boot resolves `kb_wake_cause_t`/`kb_launcher_state_t`
(`launcher_state.c`, NVS-persisted) before anything renders
(`launcher.c`'s `js_task()`). RTC clock source is `EXT_CRYS` on
watchy_v3 (real 32.768 kHz crystal, calibration logged at boot) -
confirmed accurate within 2-3s over a multi-hour test.

**Board-dependent.** watchy_v3: deep-sleep model, `KB_WAKE_COLD` /
`_RTC_TIMER` / `_BUTTON` are all real, distinct causes. waveshare_c6_amoled:
always-on model (`caps.sleep_model_deep = false`), only `KB_WAKE_COLD`
is ever actually true today - the wake-cause switch in `power_wake_cause()`
is a real switch, not hardcoded, so a future light-sleep model has
somewhere to plug in, but nothing drives it there yet.

**Missing.** Nothing at the "does it boot" level. RTC on
waveshare_c6_amoled (PCF85063, `board-bringup-notes.md`'s "next at risk"
list) isn't wired up - wall-clock time survives a *sleep* on watchy_v3
(RTC keeps ticking) but nothing on either board keeps time across a full
power-off; both rely on `atelier push`'s piggybacked `/time` POST for a
correct clock at all until an RTC or SNTP source lands.

## 2. Shows a watchface

**Stands, on both boards.** `kb_store_install_default_face()`
(`app_store.c`) self-signs and installs a real Cadran watchface
(`examples/watchfaces/default`) on first boot - unconditionally, no
network needed. `cadran_render()` draws it without booting the JS engine
on every subsequent tick (`cadran-watchface-engine.md`'s whole point).
Layout is relative to `ctx.w`/`ctx.h`, not hardcoded per board - verified
on both the 200×200 e-ink panel and the 410×502 AMOLED panel with the
same face content.

**Missing.** No face.bin caching yet - every wake still reboots the
engine to re-run `build()` before falling back to the pure-C render path
(`cadran-watchface-engine.md` §9 step 6's own flagged gap). Not wrong,
just not the optimization the design's core promise describes yet.

**A guarantee, stated as one, not left as an incidental fact** (review
round, project chat 2026-09-07): a freshly-flashed device with zero
packages installed - `kb_store_install_default_face()` never having run,
or having failed - shows `draw_no_apps_screen()` (`native-screens.md`),
a real, legible screen ("NO APPS" / "atelier push"), not a blank panel
or a crash. In normal operation this path should never actually be
seen (the default face self-installs unconditionally before this
fallback would ever trigger) - it exists specifically for the edge case
where that install didn't happen or was later erased, and the promise
is that *that* case still resolves to something coherent on screen, not
nothing.

**Board-dependent.** Which providers a face can usefully bind: `time.*`/
`date.*`/`battery.pct` work today; `steps.*` exists as a provider
(`CADRAN_PROVIDER_STEP_COUNT`/`_TARGET`, added 2026-09-07) but resolves
"unavailable" on both boards until §5 below lands real hardware. A face
binding an unavailable provider is *supposed* to degrade by skipping the
widget (`cadran-watchface-engine.md` §3) - confirmed live on both boards
2026-09-07 (§5.3 in `js-api.md`'s acceptance test).

## 3. Has a menu you can always get back out of

**Stands, partially.** Native-C `MENU` state exists
(`launcher-states.md` §1-2, hardware-verified) with real transitions:
`WATCHFACE` → `MENU` (SELECT), `MENU` → sync mode (DOWN), any non-
`WATCHFACE` state → `WATCHFACE` (BACK). `native-screens.md` inventories
today's placeholder screen. This is genuinely a *system* promise, not a
per-board detail - one state machine, `launcher.c`'s `dispatch()`, both
boards drive it.

**Missing, load-bearing.** The menu itself is a placeholder (no real
scrollable list, no watchface/app selection yet - Phase 2 below). And
critically: **"always get back out" is currently false on
waveshare_c6_amoled.** BACK is a physical-button action; touch (added
2026-09-07) only synthesizes SELECT (in `WATCHFACE`) and DOWN (in
`MENU`), nothing synthesizes BACK. Combined with the C6 never running
`EV_IDLE_TIMEOUT`'s actual effect (`app_suspend_and_sleep()` only fires
if `caps.sleep_model_deep`, which is `false` there), a touch-only board
that enters `MENU` has **no way back except a power cycle** - found live
during the js-api.md acceptance test the same day. This is the
"Zurück muss immer gehen" system promise Phase 1.3 exists to fix, not a
detail to patch inside the menu redesign.

**Board-dependent.** watchy_v3: BACK already works (a real button).
waveshare_c6_amoled: needs gesture recognition (Phase 1.1, swipe) before
BACK can exist via touch at all - there's no unused single-tap meaning
left to repurpose (§2a below, `js-api.md`'s `EV_TOUCH_TAP` already
covers "was there a tap," not direction).

## 3a. Two wake models, one state machine - a position, not just a fact

**Not previously stated anywhere, and it should be** (review round,
project chat 2026-09-07): `caps.sleep_model_deep` already exists and
already correctly describes *power* - watchy_v3 sleeps between ticks,
waveshare_c6_amoled is always-on. What it doesn't describe, and what
this doc didn't say either until this note, is that the same flag
changes what a *state* means, not just what the chip does with power in
that state.

- **watchy_v3**: "in `MENU`" is a state that *needs* a timeout. Sitting
  in a menu, awake, is exactly the condition deep sleep exists to end -
  every tick spent there without the user actually looking is battery
  the design otherwise carefully protects (`kb_power_deep_sleep()`'s
  whole reason to exist). This is already how it behaves
  (`launcher-states.md` §1's tick-wake-reverts-`MENU` rule).
- **waveshare_c6_amoled**: "in `MENU`" is a state allowed to last
  indefinitely - there's no sleep state timing out *into*, the panel is
  already going to be on regardless of what's shown on it. A timeout
  here isn't a power question at all, it's purely a UX one ("should the
  device get bored and go back to the watchface"), and nothing has
  decided that question either way.

**Why this matters for Phase 1 before it starts**: §3's "always get back
out" promise cannot be satisfied by copying watchy_v3's answer (a
timeout) onto waveshare_c6_amoled and calling it done - that would be
solving a power problem this board doesn't have while still leaving the
real one (no *explicit* exit gesture) unsolved. The C6 needs its own
answer - most likely a real BACK gesture (swipe, Phase 1.1) as the
*primary* exit, with an idle-return-to-watchface as a secondary,
UX-motivated fallback if one gets added at all - not the other way
around like watchy_v3, where the timeout *is* the primary mechanism and
BACK is the manual override.

## 4. Can select and start watchfaces and apps

**Missing entirely.** No selection UI exists. Today's only ways a face
becomes active are: (a) `kb_store_install_default_face()`'s own
first-boot install, or (b) whichever watchface package
`find_first_watchface()` (`launcher.c`) happens to enumerate first among
installed packages, persisted to NVS once chosen. Pushing a *second*
watchface and wanting to see it required a same-id-overwrite trick
(`kaliber.default`) during the 2026-09-07 acceptance tests - explicitly
flagged by both parties as a workaround for a missing feature, not a
real mechanism, and one with a real side effect:
`kb_store_install_default_face()`'s self-heal skip-guard triggers on
*any* differently-id'd watchface package existing, so leaving one
installed (even for testing) silently disables the default face's own
self-repair (`launcher-states.md`'s flagged note on this).

App selection/start has even less: `enter_app_placeholder()`
(`launcher.c`) boots *a* app if one exists, no choice of which. This is
Phase 2.2/2.3's whole scope - not designed further here.

**Board-dependent.** Not yet, because nothing exists yet to be
board-dependent about - the eventual menu is explicitly one state
machine for both input models (Phase 1.2).

## 5. Can receive packages

**Stands.** `net_svc.c`'s sync mode (menu-triggered SoftAP + HTTP
`/install`) works on both boards, hardware-verified. `install_impl()`
(`app_store.c`) verifies a package's `sig` field before trusting any of
its content - tar-structural verification happens before JSON parsing,
path construction, or flash writes touch anything the package supplied.

**Fixed 2026-09-07, real bug.** Signing used to happen at *pack* time,
baking one specific device's key into the `.comp` file - a package
signed for one device could never be installed on another, breaking
distribution outright (`package-signing.md` §2). Now: `atelier pack`
never signs; `atelier push --key <hex>` signs immediately before
sending, over the *target* device's key, read off that device's own
sync screen (now showing SSID/pass/IP/**key**, 2026-09-07). Package
format's `sig` field is `<scheme>:<key-id>:<hex-signature>`, not a bare
hex string, so a future non-HMAC scheme (real package *provenance* -
"who built this," a different, harder, deliberately-undesigned problem,
`package-signing.md` §5) doesn't need another breaking format change.

**Missing.** Sync mode is a menu action today, not itself a menu
*entry* with its own visible row (Phase 2.4). No uninstall mechanism
exists at all (`launcher-states.md`'s flagged self-heal implication -
a corrupted default face next to *any* other installed watchface repairs
itself never, today, because nothing can remove that other package).

**Board-dependent.** Not really - `net_svc.c` and `install_impl()` are
board-agnostic already; both boards proved this identically.

## 6. Sleeps

**Stands, watchy_v3 only.** Deep sleep + RTC timer wake, tick-aligned to
the real minute boundary (`kb_power_deep_sleep()`'s
`secs_to_next_minute` computation). `MENU` reverts to `WATCHFACE` on a
tick-wake (an open menu is an easy-to-forget browsing state,
`launcher-states.md` §1), stays on a button-wake. `APP` never survives a
sleep as itself - idle timeout reverts it to `WATCHFACE` first.

**Structurally doesn't apply, waveshare_c6_amoled.** Always-on by
design (`display-regions.md`'s original framing: this board is the
stress test for the HAL on the *rendering* side, not a second deep-sleep
implementation). `EV_IDLE_TIMEOUT` is a real, dispatched event on this
board too, but its handler is a no-op when `caps.sleep_model_deep` is
false - not a gap to close, a model this board was never going to have;
what its *own* power model should do on idle (dim the panel? nothing at
all yet?) isn't decided anywhere and isn't in scope for this doc either.

**Missing.** Battery/charge-status capability gates
(`power_ops_t.battery_mv`/`.charging`) are real, board-specific TODOs on
*both* boards independent of the sleep question - `js-api.md` §4's
Battery section already covers this; not repeated here beyond noting it
touches "sleeps" only insofar as a real fuel gauge would eventually want
to trigger a low-battery-specific sleep/warning path that doesn't exist
yet either.

## 7. What happens when a watchface or app crashes

**Undesigned - a real gap, not an oversight to gloss over** (flagged
2026-09-07, review round): `app_fail()` (`launcher.c`) logs the error
and sets `L.app_ok = false` - that's the entire response today. Two
different moments this can happen in behave differently, neither of
them resolved:

- **During boot** (bytecode load, `onInit`/`onResume` failing) -
  `app_boot()` returns `false`, and its one caller that checks this
  (`enter_watchface()`'s `find_first_watchface()` path) falls back to
  `draw_no_apps_screen()`. An accidental safety net, not a designed
  crash-recovery path - it only exists because that one call site
  happens to check a boolean return value, not because "what should the
  screen show after a boot failure" was ever decided as its own
  question.
- **After boot, while resident** (an `App`'s `onEvent`/`onRender`
  throwing later) - `dispatch()`'s guard (`if (L.app_ok && ...)`) stops
  forwarding further events to a failed engine, but nothing changes
  `L.state`, redraws anything, or tears the dead engine down. The device
  is left showing whatever was last successfully rendered, in a state
  that looks alive but no longer responds to anything - not a crash
  screen, not a return to the watchface, not a reboot. Worse than the
  boot-time case precisely because there's no visible signal anything
  went wrong at all.

Two different reasonable answers exist (return to `WATCHFACE`, same
`teardown_engine_if_running()` path idle-timeout already uses; or a
full restart) - neither designed nor decided here. Belongs on the list
because "what does a Kaliber device do when something goes wrong" is as
much a part of the out-of-box promise as what it does when everything
goes right, and right now the honest answer is "depends on when, and in
the resident case, not visibly anything."

## Not covered here

Look (fonts, icons, color) is explicitly Phase 4, last, and out of scope
for this inventory on purpose - a menu that works with the bitmap font
works later with a real one; the reverse isn't true (project chat
2026-09-07's own framing for the phase ordering). Gesture input
(Phase 1) and the real menu (Phase 2) are described here only in terms
of what they need to fix (§3's "always get back out" promise, §4's
selection gap) - the *how* is Phase 1/2's own design work, not this
doc's.
