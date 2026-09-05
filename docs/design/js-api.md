# JS API — sensor/device modules, Zepp-compatible

Design doc for `components/unruh/modules/`'s next layer: the sensor/
device APIs a Complication's JS actually calls (`jw.sensors.*`,
`jw.device.*`) - as opposed to `jw.ui` (already built) and `jw.watchface`
(already built, Cadran's declarative-face serializer). Per project chat
2026-09-05: the engine (QuickJS today, MQuickJS on smaller boards) is
swappable; this contract is not. Two boards already prove the *rendering*
half of that (watchy_v3, waveshare_c6_amoled - see display-regions.md
§9); this is the same discipline for the *data* half.

Status: design, no code yet. Guideline for everything below (project
chat 2026-09-05): **as Zepp OS-compatible as possible** - not
reconstructed from memory, looked up in Zepp's own current developer
documentation (docs.zepp.com, `v3+`/`@zos/*` generation - the currently
maintained one; the older `v1.0` `hmSensor` global-object generation is
noted only where it clarifies a naming choice). Closeness eases porting
a face's *logic* later; it does not mean copying Zepp's ES-module import
syntax literally - see §1.

## 1. Delivery mechanism - jw.sensors / jw.device, not `import`

Zepp OS v3+'s real shape:

```js
import { Time } from '@zos/sensor'
const time = new Time()
time.getHours()
```

Kaliber's Complications are ES5 (MQuickJS-compatible baseline, existing
project constraint - see `examples/complications/hello/app.js`'s own
header comment) loaded as a single global scope, the same way `jw.ui`
and `WatchFace({...})` already work - no module loader, no `import`.
`js_ui.c`'s own header comment already names the intended shape for
this: **`jw.sensors` / `jw.net` / `jw.storage`**, a namespace object per
category, registered only if both the manifest permission and the board
capability allow it (`js_ui.c`: *"Registered per context only if the
board has a display capability AND the manifest doesn't need a
permission for it"*) - this doc's whole "permission vs capability" split
(§3) is that same rule, generalized to categories that aren't always-on
like UI is.

Concretely, `new Time()` becomes a factory the native module returns,
not a real ES6 `class` (QuickJS supports `class`, MQuickJS's ES5 subset
may not - matching the project's existing baseline, not testing a new
one):

```js
var time = jw.sensors.Time()      // factory call, no `new`
time.getHours()
var battery = jw.sensors.Battery()
battery.getCurrent()
var vib = jw.device.Vibrator()
vib.start()
```

Method names, property names, and per-module shape below are copied
from Zepp's real signatures as closely as ES5 allows - the parts that
differ (§3's capability values, no `import`) are called out explicitly,
not left implicit.

## 2. Manifest field for required modules

Extends the *existing* mechanism (`kb_manifest_t.perm_net/perm_storage/
perm_sensors`, `app_store.c` - already parses a manifest `"permissions"`
array into these three booleans, unused by any caller yet per
`launcher.c`'s commented-out `js_register_module()` calls). Two changes
needed, not a new mechanism:

- **A new permission category, `"device"`**, for write/actuator modules
  (`jw.device.Vibrator` - the one write capability in the first wave,
  project chat 2026-09-05's own framing: *"Vibration ist bewusst dabei,
  weil es das einzige Schreibende ist"*). Lumping it under `"sensors"`
  would be conceptually wrong - a face that can *only* read shouldn't
  need to declare a write permission it never uses, and vice versa.
- **Permission stays coarse (per category), capability stays per-module**
  (§3) - a manifest says `"permissions": ["sensors", "device"]` to get
  the `jw.sensors`/`jw.device` namespaces to exist at all; it does not
  enumerate `Time`/`Battery`/`Step` individually. Finer-grained
  permissions (e.g. "sensors but not Step") are explicitly out of scope
  here - nothing in the first wave needs that distinction, and the
  existing 3-permission manifest shape doesn't have to grow just to add
  two more coarse categories.

```json
{
  "id": "de.jan.hello",
  "permissions": ["sensors", "device"]
}
```

## 3. Permission vs capability - two different failures

Two questions that sound similar and are not (project chat 2026-09-05):

- **Permission - the app isn't allowed.** Manifest doesn't declare
  `"sensors"`/`"device"` → the whole namespace object (`jw.sensors`,
  `jw.device`) **does not exist in that context at all** - same rule
  `js_ui.c` already documents for itself. `typeof jw.sensors ===
  'undefined'`, not an object with methods that throw. This is
  unchanged from what's already built - "permissions = existence", no
  runtime check to remember, none to forget.
- **Capability - the hardware can't.** Manifest *is* allowed, but this
  board has no step sensor, no vibration motor, etc. (waveshare_c6_amoled
  today: no accelerometer wired yet, see board.c's own TODOs - a step
  sensor has nothing to read from). Here the namespace/module **does
  exist** - `jw.sensors.Step` is a real, callable factory - but the
  instance reports **"not available"**, not a silently-plausible `0`.
  Concretely: `step.getCurrent()` returns `null`, not `0` - "0 steps"
  and "no step sensor" are different facts (project chat 2026-09-05's
  own framing) and a face needs to tell them apart to decide "skip this
  line" vs "show a real zero". Matches Cadran's own existing rule for
  the exact same shaped problem (`cadran-watchface-engine.md` §3: *"A
  face that binds an unavailable provider gets defined degradation: the
  widget is skipped, not an error"*) - same answer, one layer up.

Every module below states its capability gate explicitly (which
`board_desc_t.caps` field decides it) - `caps` is what a board can query
without touching JS at all, same as every other board-conditional
decision in this tree.

## 4. First wave - Time, Battery, Step, Vibrator (all pull except Vibrator)

Chosen (project chat 2026-09-05): cheap, synchronous, read-once-per-
render fits the tick model - except Vibrator, deliberately included
anyway as the one write/actuator case, so the permission split in §2-3
has a real write example from day one instead of being pure theory
until a second wave adds one.

### Time

Zepp v3+ (`@zos/sensor`'s `Time` class, `docs.zepp.com` fetched
2026-09-05): `getTime()` (UTC ms), `getFullYear()`, `getMonth()` (1-12),
`getDate()` (day of month), `getHours()`, `getMinutes()`, `getSeconds()`,
`getDay()` (1-7, 1=Monday), `getHourFormat()`/`getFormatHour()` (12h/24h).

Kaliber: `jw.sensors.Time()` mirrors these method names 1:1 - this
project already has the exact same data as Cadran providers
(`cadran/providers.c`: `time.h`/`time.m`/`time.hm`/`date.d`/`date.m`/
`date.wd`), so `jw.sensors.Time` is a thin JS-facing wrapper over the
same `cadran_provider_get()` calls Cadran's C path already makes for a
declarative face - one provider table, two callers (JS for imperative
apps, C for declarative faces), not two implementations to keep in
sync. **Always capable** (`time()` has no hardware dependency - see
`providers.c`'s own `time_set` fallback for "never synced", which
applies here too: an unset clock returns capability-available but
plausible-looking placeholder values, same `??:??`-style honesty, not
this API's problem to solve twice).

### Battery

Zepp v3+ (`Battery` class): `getCurrent()` (0-100), `onChange(cb)`/
`offChange(cb)`. **No charging-status method exists in either Zepp API
generation checked** (v1.0's `hmSensor.BATTERY` only has a `current`
property; v3+'s `Battery` class only has `getCurrent()`/`onChange()`/
`offChange()` - confirmed by reading both pages directly, not inferred).

Project chat 2026-09-05 asked for "Ladestand + lädt ja/nein" - the
charging flag is **a deliberate divergence from Zepp**, not an oversight:
`Battery.getCurrent()` matches Zepp exactly; `Battery.isCharging()`
(returns `bool`, or `null` per §3 if the board can't tell - see below)
is a Kaliber addition Zepp doesn't have. Flagged here explicitly so it
isn't mistaken for a compatibility gap later - it's the one place this
doc's own "as Zepp-compatible as possible" guideline was deliberately
not followed, because the requirement (project chat) asked for
something Zepp itself doesn't expose.

Capability gate: `board_desc_t.power->battery_mv` (already exists,
`board.h`) for `getCurrent()` - watchy_v3 has this stubbed (`TODO: read
+ calibrate PIN_BATT_ADC`, `board.c`), waveshare_c6_amoled doesn't have
it at all yet (AXP2101 not wired up, `board.c`'s own TODO) - both boards
report "not available" today, correctly, not a fake reading.
`isCharging()` needs a **new** `power_ops_t` field (`bool
(*charging)(void)`, optional/nullable like `usb_connected` already is) -
neither board can answer it yet (needs the AXP2101 on the C6, an actual
USB/charge-detect circuit on watchy_v3 beyond the existing raw
`usb_connected()` VBUS pin, which conflates "plugged in" with
"charging" - not the same fact, matching this section's own "don't
guess" rule).

### Step

Zepp v3+ (`Step` class): `getCurrent()`, `getTarget()`, `onChange(cb)`/
`offChange(cb)`. Kaliber mirrors this exactly - no divergence, no
Kaliber-specific addition.

Capability gate: **new** `board_desc_t.caps` field, `bool
has_step_sensor` (or a small sensor-capability bitmask if a second
motion-derived sensor is added before this lands - not designed further
here, per §6). Neither existing board has an accelerometer wired up yet
(watchy_v3: none in `pins.h`; waveshare_c6_amoled: none in this board's
`board.c` either, real gap not a placeholder) - `getCurrent()`/
`getTarget()` report "not available" on both until a real IMU driver
exists, same honesty as Battery above.

### Vibrator

Zepp v3+ (`Vibrator` class): `start(option?)`, `stop()`, `setMode(option)`,
`getConfig()`, plus named vibration-scene constants
(`VIBRATOR_SCENE_SHORT_LIGHT`, `..._NOTIFICATION`, `..._CALL`, etc. -
full list on the fetched page, not reproduced here since Kaliber's first
wave doesn't need scene-level fidelity yet, see below).

Kaliber's first wave: `jw.device.Vibrator()` with just `start()`/`stop()`
- no `setMode`/`getConfig`/scene constants yet. Narrower than Zepp on
purpose, not a compatibility gap: no board has more than a single fixed-
strength vibration motor today (a GPIO driving a transistor, not a
scene-programmable haptic driver chip), so `setMode`'s whole premise
(multiple selectable intensities/durations) has nothing real to select
between yet. Extending to Zepp's full scene API is mechanical *once* a
board actually has a programmable haptic driver - flagged as a known
gap, not designed further here (§6).

Capability gate: **new** `board_desc_t.power_ops_t` (or its own small
`vibrator_ops_t`, undecided - a §6 open point) field, `esp_err_t
(*vibrate_start)(void)` / `(*vibrate_stop)(void)`, both nullable.
Neither board has this wired up (no motor driver GPIO in either
`pins.h`) - "not available" on both today.

## 5. Lifecycle split - Watchface vs App

Already true today, not fully written down as a contract until now
(project chat 2026-09-05, matches Simon's js-api.md roadmap note almost
verbatim):

- **Watchface** (`WatchFace({build})`, `jw_watchface_module`): `build()`
  runs exactly once - at install, or on a manifest/ABI mismatch
  (`cadran-watchface-engine.md` §8) - and its *result* (the serialized
  widget tree) is what every subsequent minute tick renders, in pure C,
  through `cadran_render()`. **No JS runs on a tick** once `face.bin`
  exists (`launcher.c`'s `app_boot()`, wired 2026-09-05) - this is
  already Kaliber's reality, not a future plan. Consequence for this
  doc: `jw.sensors`/`jw.device` are only ever called from inside
  `build()` for a Watchface, never from a per-tick hook, because there
  is no per-tick hook - a declarative face's *rendering* reads live
  provider values in C (`cadran_provider_get()`), same data, different
  caller, per §4's Time section.
- **App** (`App({onInit, onEvent, onRender, onSuspend})`,
  `engine_quickjs.c`/`engine_mqjs.c`): resident for as long as the
  screen is showing it - MENU→APP entry through BACK/idle-timeout exit
  (`launcher-states.md` §1) - and reacts to real events
  (`onEvent`), not just ticks. This is where `jw.sensors.*.onChange()`/
  `jw.device.Vibrator().start()` actually make sense to call from -
  an App can register a callback and stay around to receive it; a
  Watchface's `build()` runs once and is gone, it has nowhere to receive
  a later callback even if it registered one.

Practical rule this doc adds: **`onChange`/`offChange` (push) callbacks
are an App-only pattern** - registering one from inside a Watchface's
`build()` is a manifest-time-detectable no-op waiting to happen
(the callback would never fire, `build()`'s JS is gone by tick 2), not a
runtime error worth spending code on. Whether to reject this at
`build()`-serialization time (`js_watchface.c`'s `serialize()` could
simply not expose `jw.sensors`'s `onChange` methods to a Watchface's
JS context at all - registering the module without those two methods,
not a permission check) or just document it is a §6 open point, not
resolved here.

## 6. Not in this design

- Push/event modules (Touch, buttons, wrist-raise) - explicitly wave 2,
  per project chat 2026-09-05: *"Frühestens sinnvoll, wenn [the
  lifecycle split, §5] steht"* - this doc gives wave 2 the contract to
  build against, doesn't design wave 2 itself.
- Zepp's full Vibrator scene API (`setMode`, `getConfig`, named
  constants) - narrowed to `start()`/`stop()` for now (§4), revisit once
  a board has a real programmable haptic driver.
- Exact `power_ops_t`/new-struct shape for `isCharging()` and the
  vibrator start/stop pair - both flagged as needing a HAL addition in
  §4, neither designed down to the struct field level here.
- Whether a Watchface's `build()` should be denied `onChange`/`offChange`
  outright vs. just documented as a no-op (§5's last paragraph) -
  flagged, not decided.
- Finer-than-category manifest permissions (e.g. "sensors but not
  Step") - nothing in the first wave needs it (§2).

## 7. Roadmap placement

Blocked on nothing already built - `jw.ui`/`jw.watchface`'s existing
registration pattern (`js_register_module()`, capability+permission
gating already proven for `jw.ui`) is the template, and both boards
this needs to work identically on already exist and boot the same
firmware (display-regions.md §9). Suggested order once this doc is
agreed, per project chat 2026-09-05:

1. Lifecycle split (§5) made explicit in code, not just true in
   practice - the manifest-time `onChange`-in-`build()` question (§6)
   settled here, before any module ships to make it a live footgun.
2. First-wave modules (§4): Time (always capable, no HAL work), then
   Battery/Step/Vibrator (each needs its own `board_desc_t`/`power_ops_t`
   addition, §4's per-module capability-gate notes) - Time first proves
   the registration/permission/capability plumbing end to end against
   the one module with zero hardware dependency, same sequencing
   logic `launcher-states.md` used (state model before the menu that
   needs it).
3. A real face using at least one pull module, on both boards - the
   same "prove it, don't assume it" bar display-regions.md §9 step 4
   already set for rendering, applied to data next.
