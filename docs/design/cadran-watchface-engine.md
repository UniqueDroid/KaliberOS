# Cadran — the Kaliber watchface engine

*Cadran* (French: the watch dial) renders JS-defined watchfaces. The design
follows the Zepp OS watchface model — declarative widgets bound to system
data — but exploits Kaliber's deep-sleep model for an optimization Zepp OS
never needed: **on a minute tick, the JS engine does not boot at all.**

Status: design. Target component: `components/cadran/`.

## 1. The core trade

A Zepp OS watchface is mostly declarative: widgets (image digits, hands,
arc progress, date images) are created once and bound to data sources; the
runtime updates them without app code. Cadran pushes this further:

- `build()` of a face runs **exactly once** — at install time, or after the
  user edits the face. It returns a widget tree.
- The firmware validates the tree and serializes it to a compact binary
  blob, `face.bin`, stored next to the app in `/apps/<id>/`.
- Every subsequent minute tick is pure C:

  ```
  wake (RTC timer)
    -> load face.bin from LittleFS
    -> pull bound provider values (time, date, battery, ...)
    -> render widgets into the canonical framebuffer
    -> e-ink partial refresh
    -> deep sleep
  ```

No engine init, no bytecode load, no JS heap. This makes declarative faces
the primary format on RAM-constrained boards (Watchy v2, C6) — the C
renderer runs everywhere regardless of which engine the board carries —
and cuts wake latency and energy on every board.

## 2. Face classes

| class       | JS at runtime                  | use case                        |
|-------------|--------------------------------|---------------------------------|
| declarative | never (after build)            | default; all standard faces     |
| hybrid      | button wake / edit only        | custom region atop widget tree  |
| imperative  | every wake (`App.onRender`)    | escape hatch, full jw.ui access |

Hybrid faces declare the widget tree for tick rendering plus `onEvent` /
`onRender` for interaction; minute ticks stay JS-free, button wakes boot
the engine. The manifest declares the class:

```json
{ "type": "watchface", "face": { "mode": "declarative" } }
```

## 3. Face API (JS)

```js
WatchFace({
  build: function (ctx) {
    // ctx: { w, h, disp: "eink1" | "rgb565", caps: ["battery","steps",...] }
    return {
      widgets: [
        { type: "img",        x: 0,   y: 0,   src: "bg" },
        { type: "img_digits", x: 30,  y: 60,  set: "digits_lg",
          bind: "time.hm", layout: "HH:MM" },
        { type: "text",       x: 20,  y: 150, font: "sys8",
          bind: "battery.pct", format: "{v}%" },
        { type: "arc",        x: 100, y: 100, r: 92, w: 6,
          bind: "battery.pct", min: 0, max: 100 },
        { type: "hand",       x: 100, y: 100, len: 70, width: 3,
          bind: "time.min_angle" }
      ],
      aod: [ /* optional reduced tree; ignored on e-ink boards */ ]
    };
  }
});
```

Rules:

- `build()` must be pure: no `jw.net`, no timers, no side effects. The
  serializer rejects trees referencing resources not present in the
  package.
- Conditional layout is allowed *inside* `build()` (branch on `ctx.w`,
  `ctx.caps`) — the result is still a static tree per device.
- A face that binds an unavailable provider gets defined degradation: the
  widget is skipped, not an error. `ctx.caps` lets `build()` substitute
  layouts instead.

## 4. Widget set v1

Deliberately close to the Zepp vocabulary:

- `img` — static image.
- `img_digits` — number rendering from an image set (the classic Zepp
  `IMG_TIME`/`TEXT_IMG` pattern). `layout` uses placeholder chars
  (`HH:MM`, `DD.`, `W`).
- `text` — built-in bitmap font + `format` string with `{v}`. `font` is a
  **role**, not a pixel size (§5a) — `"sys8"` in the example above predates
  the role naming below and should read as `"small"`.
- `arc` — arc progress, `bind` + `min`/`max`.
- `img_level` — one image out of N by value range (battery bars, moon
  phases).
- `hand` — analog hand, rotated **as a polygon in C** (no rotated image
  assets; matters on 1-bit e-ink).
- `rect`, `line` — primitives for minimal faces.

Out of scope for v1 (reserved keys): `edit_slot` (Zepp-style on-watch
editable groups), animations.

## 5. Data providers (C side)

Providers are registered in the firmware and capability-gated by the HAL:

- always: `time.h`, `time.m`, `time.hm`, `time.min_angle`,
  `time.hour_angle`, `date.d`, `date.m`, `date.wd`, `battery.pct`
- gated by `caps` (future): `steps`, `hr`, `stress`
- `app.N` — up to 8 numeric/string slots a hybrid face's JS can write
  (e.g. fetched weather); persisted with app state, rendered by the C
  path on subsequent ticks. This is the bridge that keeps even
  data-driven faces JS-free on minute ticks.

Provider reads happen at render time in C; there is no dirty tracking in
v1 (a full widget re-render per tick is cheap relative to the e-ink
refresh).

**One source, not two** (project chat 2026-09-05, once `js-api.md`
gave the same hardware a second facade, `jw.sensors`/`jw.device` for
imperative Apps): a provider here must call the exact same
`board_desc_t` op (`power_ops_t`/`sensor_ops_t`/`vibrator_ops_t`) the
matching JS module calls, never read the sensor/PMIC itself - the way
`cadran_provider_get(CADRAN_PROVIDER_BATTERY_PCT, ...)` is the thing
`jw.sensors`'s `Battery.getCurrent()` calls, not a second mV→% reader.
A capability-absent provider (steps, once added) fails the *identical*
NULL-check its JS counterpart fails, for the same reason - see
`js-api.md` §4a for the full "one source, two facades" rule this
section is one half of, including why §3's skip-not-error rule (not a
placeholder) is the answer for every capability-absent provider except
the one documented exception (`time.hm`'s `"??:??"`, immediately
below).

## 5a. Font roles, not pixel sizes

(project chat 2026-09-30, following the gfx bitmap-font work in
`components/gfx/` - `gfx_font16x16.h`/`gfx_font32x32.h` today, a larger
digits-only size next.) A `text` widget's `font` field is one of a small,
fixed set of **roles** - `"large"` / `"medium"` / `"small"` - never a raw
pixel value. The reasoning is the same one `build()`'s own `ctx.w`/`ctx.h`
branching already exists for: a face is one `face.bin`, shared across
boards with wildly different panels (200 px, 410-502 px today, more
later). A face that requested "48 px" would be right on one board and
wrong on every other - a role lets the *renderer* pick the concrete font
for *this* panel, keeping the same `face.bin` valid everywhere, which is
the whole point of building faces once at `build()` time instead of per
board.

Resolution happens in `cadran_render()`, per stripe, the same place
`launcher.c`'s `headline_font()` already does this for native screens
(§8's `draw_menu_placeholder()` etc. - that helper is this rule's first,
smaller instance, not a separate mechanism): pick the largest font
registered for the requested role that still fits this board's panel for
the widget's actual rendered string (width-fit check against
`ctx->board->caps.disp_w`, same as `headline_font()`).

**Fallback is mandatory, not an error path**: if the size a role would
naturally resolve to doesn't exist on this board (not registered) or
doesn't fit (string too wide for the panel at that size), the renderer
steps down to the next-smaller role and retries - `large` → `medium` →
`small`. `small` (the 16x16 font, always registered) is the floor and
always fits, the same way `gfx_draw_text()`'s original 8x8 font always
did - a face never fails to render for a font reason, it just renders a
size smaller than requested. This mirrors §3's "skip, don't error" rule
for capability-absent providers: a size that isn't available degrades,
it doesn't break the face.

Which concrete `gfx_font_t` backs which role, and whether a role's font
is full-ASCII or a restricted charset (digits+colon only, for flash cost
- large is the size most likely to be digits-only, since a `text` widget
bound to `time.hm` never needs letters), is a firmware-side registration
table, not part of `face.bin` - a face names a role, never a font table
directly, so adding/re-tuning sizes later doesn't touch any already-
serialized face.

Not yet implemented: `face_format.h`'s widget record still only carries
`params[0]` as a plain scale factor (`render.c`'s `draw_text()`). This
section documents the target shape; the change is `params[0]` becoming a
role enum (0=small/1=medium/2=large, small as the zero-value default so
an old/hand-written test blob with `params[0]==0` keeps working
unchanged) plus the resolve-with-fallback loop above in `draw_text()`.
`js_watchface.c`'s JS-side `font: "large"/"medium"/"small"` string→enum
mapping is the matching half, same shape as its existing provider-name
table.

## 6. face.bin format

Little-endian, versioned (`CADRAN_ABI`, independent of the JS bytecode
ABI — a face survives engine swaps):

```
header:  magic "CDRN", u8 abi, u8 widget_count, u16 flags
strings: interned string table (resource names, formats)
widgets: fixed 24-byte records: type, x, y, params[4], bind_id, str_refs
```

Target: a typical face < 1 kB. Loading is a single read + pointer fixup —
no parsing. Validation happens once at serialization time, so the tick
path can trust the blob (it is still bounds-checked defensively; it lives
on writable flash).

## 7. Resources & atelier

`atelier pack` gains a resource pipeline. Faces ship a `resources/`
directory; atelier converts every image **at pack time** into the native
format of the target display declared by the board profile:

- e-ink boards: 1-bit, Floyd–Steinberg dithered, packed rows
- RGB565 boards: 16-bit, optional RLE

The device never decodes PNG — no decoder in firmware, no decode RAM
peak, dithering happens where compute is free. Consequence: a `.comp` is
built *per display class*; atelier emits `res.eink1/` and `res.rgb565/`
variants and the installer keeps only the matching one (mirrors the
existing per-engine bytecode entries).

Digit sets (`img_digits`) follow the Zepp convention: one image per glyph
(`0.png` … `9.png`, `colon.png`), atelier packs them into a single strip
with an index.

## 8. Launcher & power integration

- The launcher checks `face.bin` presence: if the current app is a
  declarative/hybrid face and the wake cause is `KB_WAKE_RTC_TIMER`, it
  calls `cadran_render()` and never touches Unruh. Button wakes on hybrid
  faces boot the engine as today.
- After install or edit, the launcher boots the engine once, runs
  `build()`, serializes, stores `face.bin`, and tears the engine down.
- Success criterion 2 (wake→display < e-ink refresh) becomes trivially
  true for declarative faces; measure and log the C-path render time
  separately (target: < 10 ms for a 5-widget face on 240 MHz).

## 9. Roadmap placement

Cadran slots in after the SSD1681 driver and the font rasterizer (it
needs both) and *before* the MQuickJS backend — declarative faces reduce
the urgency of the second engine, since v2/C6 boards can run every
declarative face with no JS engine involvement at all. Suggested order:

1. [x] `cadran_render()` + face.bin loader (C only, hand-written test blob)
2. [x] widget renderers: rect/line/hand, text (img/img_digits/arc/img_level
   still skipped - need the atelier resource pipeline, step 5). Hardware-
   verified 2026-09-03 via `cadran_selftest()`'s optional panel blit
   (`CADRAN_SELFTEST_BLIT_TO_PANEL`, off by default) - real e-ink showed
   the test rect/line/hand/text, not just a RAM pixel check.
3. [x] provider registry (time/date/battery)
4. [x] serializer in firmware + `WatchFace()` global in Unruh
   (`components/unruh/modules/js_watchface.c`). Hardware-verified
   2026-09-03 via `js_watchface_selftest()`: a real face.js
   (`examples/watchfaces/simple/face.js`) calling `WatchFace({build})`,
   serialized, engine torn down, then rendered through
   `cadran_render()` with no engine involved - the design's core
   promise (§1), end to end. Not wired into the launcher yet (step 6);
   `build()`'s trigger (face.bin missing/ABI mismatch -> boot engine,
   run build(), serialize, destroy engine) is still only exercised by
   the self-test harness.
5. atelier resource pipeline (dithering, digit strips)
6. [x] launcher wiring (`launcher.c`'s `app_boot()`), 2026-09-05 - a
   package whose bytecode calls `WatchFace({...})` instead of `App({...})`
   now boots the engine, runs `build()`, serializes, renders through
   `cadran_render()`, and tears the engine down again, all within one
   `app_boot()` call. Real gap versus this section's design: **no
   face.bin caching yet** - every wake re-runs `build()` and reboots the
   engine, it does not yet skip Unruh entirely on a tick the way §1's
   core promise describes. Correct behavior, missing optimization; a
   real face.bin-presence check (build() only on install/ABI mismatch,
   as designed above) is still open. Also lands the default out-of-box
   face this step existed to enable (§4 below, `examples/watchfaces/
   default`) - auto-installed on first boot via
   `kb_store_install_default_face()` (`app_store.c`), signed at runtime
   with this device's own key (§4's real mechanism, not a fixed key).

Step 1–3 need no JS at all and can be built against the hello example's
partition layout — good parallel track while the QuickJS build lands.

**Not yet started:** a `CADRAN_PROVIDER_STEP_COUNT` (and likely
`_TARGET`) provider - needed for the js-api.md acceptance-test face
(time+battery+steps on both boards) and the first real exercise of
§5's "one source, not two" rule added above, since `jw.sensors.Step()`
(`unruh/modules/js_sensors.c`) already exists and reads
`board_desc_t.sensors` directly - the new provider must call the exact
same `sensor_ops_t` fields, not reimplement the read.
