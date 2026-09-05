# Launcher states — watchface, menu, apps

Design doc for the layer above Cadran: what's on screen, who chose it,
and how the user gets to a menu and back. Today's launcher
(`components/launcher/launcher.c`) just boots `kb_store_list()[0]` -
bring-up logic, not a concept. Priority: before the second board (no
watchface state, no real HAL stress test worth doing yet - "KaliberOS is
still an app loader, not a watch").

Status: design, no code yet.

## 1. State model

Three states, matching the task's Zepp-OS-inspired framing and (per the
task) the original Watchy firmware's `guiState`, used as a reference
precedent, not copied wholesale:

```
WATCHFACE (base state)         MENU                        APP
  - always exactly one active    - list of installed          - the selected
    face                           complications                complication,
  - never "started", always       - UP/DOWN navigate,           foreground
    what a tick renders             SELECT enters MENU          - BACK -> WATCHFACE
  - BACK: no-op                   from WATCHFACE               - idle timeout:
                                 - SELECT on an item:            same as WATCHFACE
                                   -> APP                        (suspend + sleep)
                                 - BACK: -> WATCHFACE
                                 - idle tick while in MENU:
                                   -> WATCHFACE (see below)
```

`kb_app_type_t` (`KB_APP_WATCHFACE` / `KB_APP_APP`, already in
`kb_manifest_t`, currently unread anywhere) is what MENU filters on to
build "choose a watchface" vs. the general app list (§3).

**What survives deep sleep, and Watchy's auto-revert rule.** The task
offers NVS or `RTC_DATA_ATTR`; this doc picks **NVS**, for one concrete
reason: this project has no `RTC_DATA_ATTR` usage anywhere yet, while
the exact "small value, written rarely, read on boot" shape already has
two working precedents in this codebase - `get_hmac_key()` and
`net_svc.c`'s `ap_password()` (both NVS get-or-generate). Reusing that
pattern costs nothing new to reason about or test; `RTC_DATA_ATTR` would
be a second persistence mechanism to maintain for a state transition
that (MENU→APP, BACK, idle-tick-revert) happens rarely enough that flash
write frequency was never a real constraint here. The one place NVS is
*stronger* than `RTC_DATA_ATTR` - surviving a full power cycle (battery
pull, not just deep sleep) - turns out to be the right default anyway: a
watch that comes back from a dead battery landing in WATCHFACE rather
than wherever the menu was left is the safer failure mode, not a cost.

Watchy's rule - the menu automatically falls back to the watchface after
a tick, so the device never gets stuck showing a stale menu to nobody -
is adopted as-is, and this project already has the exact primitive it
needs: `kb_power_wake_cause()` already distinguishes
`KB_WAKE_BUTTON` from `KB_WAKE_RTC_TIMER`. The rule becomes: **if the
persisted state is MENU and the wake cause is `KB_WAKE_RTC_TIMER`,
revert to WATCHFACE before rendering anything** - no new capability
needed, just a check at the top of `js_task()` using what's already
there.

**APP does not get the same auto-revert.** A running Complication (a
game, a stopwatch, a timer) is something the user is actively using, not
idly browsing - a plain minute tick shouldn't yank them out of it. APP
reverts to WATCHFACE via the *existing* idle-timeout mechanism instead
(`EV_IDLE_TIMEOUT`, already implemented, already what triggers
`app_suspend_and_sleep()` today) - no new timer, just routing that
existing event to "return to WATCHFACE" when the current state is APP,
instead of straight to sleep. WATCHFACE itself needs no revert rule at
all: it's the state everything else falls back to, there's nothing above
it to fall back further.

## 2. Persisted selection

One NVS value: the active watchface's `id`. Boot with no stored
selection (first boot, or the stored id no longer exists - e.g. removed
via a later push): fall back to the first installed package with
`type == "watchface"`. None of those either: the existing NO-APPS screen
(`components/launcher/launcher.c`, added 2026-09-04 alongside removing
`seed_apps.c`) - unchanged, this doc doesn't need a new empty state,
that one already covers "nothing to show."

"Choose a watchface" (a MENU entry per the task, §3) writes the same NVS
value. No separate "last app" tracking needed for APP - by design (§1),
APP is always reached by explicitly picking it from MENU, never resumed
directly on wake.

## 3. Menu

A list of installed complications (from `kb_store_list()`, filtered by
whether the state model asked for "everything" or "watchfaces only" -
§1's "choose a watchface" entry), UP/DOWN moves a highlighted selection,
SELECT enters (a watchface selection writes NVS + returns to WATCHFACE;
an app selection enters APP). Rendering: a scrollable text list is
squarely within what `gfx`/Cadran's existing primitives already do
(`rect` for a highlight bar, `text` for labels) - no new drawing
capability needed for v1.

**Native C or a bundled Complication - decided: native C.** Three
reasons, all grounded in how this codebase already draws this exact line
elsewhere, not argued from first principles:

1. **Must work when the store can't help it.** Every "must always work
   regardless of installed-app state" screen this project has built so
   far - `cadran_selftest()`, `js_watchface_selftest()`, the NO-APPS
   screen, `net_svc.c`'s sync-mode screen - is native C, specifically
   *because* it can't depend on there being a working, installed,
   correctly-signed app to fall back on. The menu is the same category:
   if it were itself a Complication and that package failed to install,
   got corrupted, or trips a future ABI bump, the device would lose its
   *only way to navigate to anything else* - a materially worse failure
   than losing one user app. A native menu keeps working precisely when
   the store is empty or unhealthy, which is exactly when the user needs
   it most.
2. **No engine boot to show a list.** Keeping with the project's
   existing "boot the engine only when truly needed" posture (the whole
   reason Cadran's declarative faces exist), a bounded list-of-installed-
   apps display doesn't need the flexibility a JS engine buys - it needs
   a scrollable list and a highlight, which C already draws today.
3. **It's navigation chrome, not user content.** Complications are
   optional, sandboxed, removable, updatable independently via `atelier
   push`. The menu is the thing you use to *get to* a Complication - closer
   to a phone's home-screen/status-bar layer than to an installed app,
   and this project has consistently kept that layer in firmware, not in
   the app runtime.

The real counterargument - "a Complication is updatable without a
reflash, and building the menu as one proves the platform's own APIs are
expressive enough for real UI" - is worth taking seriously, not just
overridden. It doesn't change the recommendation, because it's solving a
*different* problem than robustness: nothing here rules out later making
the menu's *appearance* (not its logic) data-driven, similar in spirit
to Cadran's `face.bin` - a small config/theme blob the menu's native
renderer reads, updatable without a firmware push. That's explicitly not
needed for v1 (no touch gestures, no theming in scope either) and isn't
designed here, just noted as the compromise path if "the menu never
changes without a firmware update" becomes a real complaint later,
rather than something this doc needs to solve now.

## 4. Default face - what a fresh watch shows

**Status (2026-09-05): implemented per this section's actual recommendation
below**, not the fixed-key shortcut an earlier same-day pass used.
`examples/watchfaces/default` + `kb_store_install_default_face()`
(`app_store.c`) installs a real Cadran clock face on first boot: embeds
only the *unsigned* manifest.json + bytecode (`default_face_pkg.h`, no
baked signature anywhere in the repo), signs them at runtime with
`get_hmac_key()`'s own output (this device's real per-device key, or the
Kconfig override on fleets that provision one - the function doesn't
care which), assembles a tar container, and installs it through the
unmodified `kb_store_install()` path - the exact chain described below,
literally, not just in spirit. Verified live: a device that already had
the earlier fixed-key-signed copy got it silently replaced with a
properly-signed one on its next boot (the "already installed" guard
only skips for a package with a *different* id).

The constraint from the task: a freshly flashed watch shows the time,
not nothing - but not via `seed_apps.c`'s pattern (bytecode written
straight into `/apps/<id>/`, no manifest, bypassing
`kb_store_install()` entirely), which is what just got removed as the
last bring-up hack in the tree (2026-09-04, project chat).

The task's own proposal - embed a real `.comp`, install it through the
actual `kb_store_install()` path on first boot, not a bypass - is the
right shape, but has a real gap worth stating plainly rather than
glossing over: **`kb_store_install()` requires a valid HMAC signature
against *this device's own* key, and that key doesn't exist yet at
firmware build time.** `get_hmac_key()` generates a fresh, random,
per-device key into NVS on first use (`app_store.c`) - a signature baked
into the firmware image at build time (the way
`store_install_selftest_pkg.h`'s embedded test package is signed with a
fixed `KALIBER_STORE_HMAC_KEY_OVERRIDE` test key, gitignored precisely
*because* it's tied to one specific key) would only verify on devices
sharing that same fixed key, i.e. a fleet-provisioned override - not the
normal per-device-key path every real device actually uses.

**Resolution: sign at first boot, with the device's own key, then feed
the result through `kb_store_install()` completely unmodified** - this
is what actually satisfies "uses the real install chain, not a bypass,"
literally, not just in spirit. Concretely: embed the *unsigned* pieces
(`manifest.json` text + bytecode, the same two byte arrays
`store_install_selftest_pkg.h` already demonstrates how to generate),
and on first boot - after `get_hmac_key()` has a real key to work with -
compute the HMAC over them the same way `atelier.py`'s `cmd_pack` does
(manifest + bytecode entries, sorted by name), assemble a small tar
container with the result, and call `kb_store_install()` on it like any
other package. No signature verification is skipped, no new bypass path
is added to `kb_store_install()` itself - the "trust" question moves to
"is this build's embedded manifest/bytecode trustworthy," which is the
same trust boundary as the rest of the firmware image (anyone who can
alter the embedded bytes can alter the firmware itself; HMAC-checking a
file built from your own already-trusted source tree adds nothing a
compromised build wouldn't defeat anyway).

Because the embedded artifact needs no fixed test key, it also doesn't
inherit `store_install_selftest_pkg.h`'s reason for being gitignored -
this one can be committed to the (public) repo like any other example.

**What the face actually is - and a real dependency this creates.** A
plain imperative `App()` "clock" isn't buildable today without also
inventing a new `jw.time` module (no imperative app currently has any
way to read the real clock - `jw.ui` only offers `clear`/`text`/
`invalidate`). A Cadran declarative face, by contrast, already has
working `time.hm`/`date.*` providers (Cadran doc §5, roadmap step 3,
done) - showing the real time through Cadran needs *no* new JS-facing
API. Recommended default face: a small declarative Cadran face (a clock,
using providers that already exist), not a new imperative example.

The real dependency this creates: Cadran's own design doc already
specifies how an installed declarative face becomes renderable -
"after install... the launcher boots the engine once, runs `build()`,
serializes, stores `face.bin`" (Cadran doc §8) - that was roadmap step 6,
landed 2026-09-05 (see the status note above and Cadran doc §9's roadmap
table) with one caveat of its own: no `face.bin` caching yet, so every
wake re-runs `build()` rather than skipping Unruh on a tick the way
Cadran §1's core promise describes. Sequencing note kept for history:
§1-3 were built and verified against plain imperative apps (`hello`,
still in `examples/complications/`) before Cadran step 6 landed, exactly
as planned here.

## 5. Where Cadran's hybrid boundary meets this one

Cadran's design doc already answers "when does the engine boot for a
watchface" - a hybrid face's minute ticks stay JS-free, a button wake
boots the engine for `onEvent`/`onRender` (Cadran doc §2). That's an
*internal* detail of the WATCHFACE state, not something this document
redefines: interacting with a hybrid face (e.g. a button press revealing
a secondary complication slot) stays inside WATCHFACE, it is not a
transition to MENU or APP. The boundary this document adds is the outer
one - WATCHFACE (declarative-only, or hybrid-with-occasional-engine-use)
vs. MENU vs. APP - layered on top of, not overlapping, Cadran's own
already-settled tick/interaction split.

**Does the engine stay loaded across the states this document adds?**
Yes, within one wake session, uniformly: booting into MENU or APP boots
the engine (if it isn't already up - a hybrid interaction may have just
booted it, in which case MENU/APP reuse that instance rather than
tearing down and rebooting), and it stays resident through MENU
navigation and however long the user stays in APP - same as today's
single-app model, no new churn per button press. Teardown happens at
exactly two points, both of which already exist as a pattern:

- **Idle timeout** (`EV_IDLE_TIMEOUT`, existing): tears down and sleeps,
  same as today, from any state.
- **Explicit return to WATCHFACE** (BACK from MENU with nothing entered,
  or an app's own exit path) - a *new* teardown point this document
  adds: tear down the engine immediately and re-render WATCHFACE, rather
  than waiting for the next idle timeout. This is the same
  `js_destroy()` call `app_suspend_and_sleep()` already makes, just
  without the following `kb_power_deep_sleep()` - reclaims the JS heap
  budget the moment the user is back to "just looking at the time,"
  which is also exactly the condition under which a declarative face's
  *next* tick can go back to being fully engine-free, Cadran's central
  promise (Cadran doc §1). Getting back to that state promptly on every
  MENU/APP exit is a real reason to eagerly tear down here rather than
  defer to the idle timer, not just tidiness.

## 6. Not in this design

Implementation, touch gestures, the second board (all per the task).
Also not decided here, deliberately: menu *theming* beyond "a scrollable
text list with a highlight" (§3's compromise-path note), and anything
about how `jw.time` (or any other new imperative-only capability) might
eventually work - out of scope because §4's recommendation avoids
needing it for the default face specifically.

## 7. Roadmap placement

Priority: before the second board (task's framing - "without watchface
state this is an app loader, not a watch"), and per Jan's ordering note
(project chat 2026-09-04): the `display_ops_t` refactor
(`docs/design/display-regions.md`) is small and behavior-neutral, do
that first; this document's state model comes after it and before the
C6 board itself.

1. State machine (§1) + NVS persistence (§2), verified against the
   existing `hello`/`budget-hog` examples - no Cadran or menu-rendering
   dependency, can be built and tested standalone.
2. Native menu (§3) - the first real user of `gfx` beyond bare test
   text, and (per the task) the first real test of whether `jw.ui` and
   the striped rendering from `display-regions.md` actually compose -
   worth sequencing after that design's own roadmap step 1-2 (Watchy
   ported to the new `display_ops_t`, verified behavior-neutral) even
   though the menu itself ships on Watchy first, so the rendering path
   it exercises is already the one the C6 will use, not a second one to
   port later.
3. Cadran roadmap step 6 (hybrid wiring: install-time `build()` +
   `face.bin` caching) - blocks §4 specifically, nothing else here.
4. Default face (§4), once step 3 lands.
5. Second board.
