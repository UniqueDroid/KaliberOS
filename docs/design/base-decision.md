# Base decision — pull LVGL into KaliberOS, or KaliberOS into esp-watchos?

A comparison, not a plan (project chat 2026-09-07): which codebase should
be the foundation going forward - KaliberOS's own `gfx`/Cadran/`board_hal`
stack with LVGL pulled in for fonts/icons/gestures, or Jan's separate
`esp-watchos` project (same ESP32-C6-Touch-AMOLED-2.06 hardware, already
running LVGL + ESP-Brookesia) with Kaliber's JS layer built into it? No
code changes here - this is the decision itself.

## 1. What's actually finished in esp-watchos

Verified against the real source (`~/Projekte/esp-watchos`), not
recalled from memory:

| Area | State |
|---|---|
| Boot, power/boot button | Runs on device - `main/main.cpp`'s boot sequence, BOOT (GPIO9) and a second physical button toggle AOD/home screen, both polled and hardware-confirmed working (see `board-bringup-notes.md`'s own citation of this project's button-polarity lesson). |
| AOD clock (default watchface) | Runs on device - a real screen (weekday/date, battery, weather, WiFi status), replaced an earlier dedicated "watchface app" (git history: `9d08a81 rework AOD clock layout, remove the watchface app`). Not a Cadran-style declarative face - an LVGL screen updated on a 1 Hz timer. |
| Menu / app switching | Runs on device - ESP-Brookesia's phone-style launcher (home screen, app icons, status bar), 8 real apps (WiFi Connect/Analyzer, Signal Tracker, Network Scanner, Port Checker, Timer, Flappy, Settings) plus AOD, each a self-registering C++ class. |
| Settings | Runs on device - device settings screen, WiFi credential management (AES-128-CTR encrypted at rest), a settings webserver with HTTP Basic Auth. |
| Touch/gesture input | Runs on device - LVGL's own `indev` system via `esp_lvgl_port_add_touch()`, real tap/swipe/long-press already built into LVGL, not hand-rolled. |
| Fonts | Exists in code, built-in only - LVGL's bundled Montserrat bitmap font, several sizes + a compressed variant (`sdkconfig.defaults`). No custom TTF rasterizer, no vector fonts. |
| Icons / image pipeline | Exists in code, no real pipeline - each app ships one PNG converted once, offline, to a C byte array via LVGL's own image-converter tool (`LV_IMG_DECLARE`/`lv_img_dsc_t`, checked into each app's own `assets/`). Not an atelier-style build-time pipeline - a manual, per-asset, one-off conversion step. |
| Dynamic app install | **Missing entirely.** Apps are native C++ classes, compiled into the firmware image, self-registered at static-init time (`ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR`). No store, no package format, no signing, no OTA/push-install path of any kind - confirmed by grep, nothing in `components/system/` implements this. |
| Sleep / power model | **Missing entirely, by design.** `main.cpp`'s own comment: "No CPU/light sleep involved - just a normal 1 Hz [timer]." Always-on only, same as Kaliber's own `waveshare_c6_amoled` today - this project has never had to solve a deep-sleep-vs-always-on split because it only targets one always-on board. |
| Second board / e-ink | **Missing entirely.** C6-AMOLED only, no board abstraction layer at all - one board, one firmware, no HAL boundary to speak of. |
| Sensors (step/IMU, etc.) | Not checked in detail (out of scope for this comparison - no capability/permission model exists to compare against either). |

## 2. How the graphics stack is built, and whether it's separable

**LVGL 9.5.0 directly** (`managed_components/lvgl__lvgl`), wired to the
panel/touch hardware through Espressif's `esp_lvgl_port` managed
component (`lvgl_port_add_disp`/`lvgl_port_add_touch` - handles buffer
allocation, the flush callback, and touch-to-`lv_indev_t` plumbing).
**ESP-Brookesia** sits on top as the app-lifecycle/window-manager layer
(home screen, status bar, app run/back/pause/resume/close) - vendored as
real source under `components/system/brookesia_core/`, not a thin
dependency; every app in `components/apps/` subclasses Brookesia's
`App` base class directly.

**Buffer size, measured from the actual config**: a single (not double)
draw buffer, `40 lines × 410 px × 2 B (RGB565) = 32,800 B` -
deliberately reduced from LVGL's own 100-line default (~80 KB) via
`CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=40`, and the config comment says
why: "the single largest static RAM consumer on this no-PSRAM board."
No PSRAM exists on this chip at all (`sdkconfig.defaults`: "ESP-IDF v6.0
has no PSRAM/SPIRAM Kconfig support for ESP32-C6 at all... All heap is
internal SRAM") - the same hardware fact `waveshare_c6_amoled/board.c`
already documents for Kaliber's own build.

**Separable or not**: LVGL itself is architecturally separable from
Brookesia - Brookesia is built *on* LVGL's public API, not the reverse,
so LVGL alone (display + touch + widgets, no app-lifecycle opinions)
could in principle be pulled out cleanly. **Brookesia is not separable
from esp-watchos's actual UI** - the home screen, app switching, and
every screen a user sees is a Brookesia `App`, so reusing esp-watchos's
*look and interaction*, not just its rendering library, means taking
Brookesia too, with everything that implies for Weg B below.

## 3. The two efforts, side by side

### Weg A — pull LVGL into KaliberOS

What would need to change:

- **`display_ops_t`/`gfx_ctx_t`**: LVGL wants to own its own flush
  callback and either a full frame buffer or its own dirty-rect/partial-
  render bookkeeping - not Kaliber's caller-imposed fixed-height
  horizontal stripe loop. `blit_region()` maps plausibly onto LVGL's
  flush callback (a real integration point, not a dead end), but
  `caps.stripe_lines`'s whole reason to exist (fit a huge panel's frame
  in RAM by rendering it in bands the *caller* controls) would need to
  coexist with LVGL deciding its own redraw regions - two overlapping
  ideas about who owns "what gets redrawn when," not obviously
  reconcilable without real design work.
- **Execution model**: LVGL expects a continuously-running task calling
  `lv_timer_handler()` - Kaliber has no persistent render task today
  (native-C screens draw synchronously inside `dispatch()`; Cadran
  renders once per tick and stops; the JS engine boots and tears down
  per app-lifecycle). Anything LVGL-based would need to keep a task
  alive the whole time it's on screen - fine on the always-on C6,
  directly opposed to watchy_v3's entire deep-sleep model (nothing runs
  between ticks there, by design).
- **Cadran**: no natural LVGL equivalent exists for "serialize a widget
  tree once, render it from a tiny blob in pure C on every tick, no
  engine, no retained object graph." LVGL is a full retained-mode scene
  graph - the architectural opposite. Keeping both means two rendering
  paradigms running side by side, not one; replacing Cadran with LVGL
  screens would mean giving up the specific property
  (`cadran-watchface-engine.md`'s whole premise, hardware-proven this
  session on both boards) that makes a watchface cheap enough to render
  every minute tick without booting anything.
- **watchy_v3 (e-ink)**: LVGL has some monochrome/e-ink support but it's
  a less-mature, more specialized path than its RGB565 story: full-vs-
  partial refresh, ghosting mitigation (the exact SSD1681 counter
  `board.c` already carries) would need real re-integration, not a
  drop-in.

**Rough estimate**: 10-15 days for a first working integration limited
to the C6 (flush-callback adapter, a minimal always-on task model,
*not* touching Cadran or watchy_v3) - genuinely open-ended beyond that,
because reconciling LVGL's execution model with watchy_v3's deep sleep
is a real design question, not an implementation detail, and might not
resolve to "one shared stack" at all.

### Weg B — build KaliberOS's JS layer into esp-watchos

What would need to go in:

- **Unruh (QuickJS lifecycle)**: esp-watchos has no JS engine today -
  a wholesale new subsystem, but Unruh itself is small/self-contained.
  ~2-3 days to get it building and running standalone inside
  esp-watchos's own build.
- **`app_store.c` (tar parsing, HMAC verification, manifest)**: also
  fairly portable C, needs adapting to esp-watchos's own LittleFS
  layout (`os_fs`) instead of Kaliber's `ROOT`/`apps` convention.
  ~2-3 days.
- **Cadran**: the harder piece. esp-watchos's screens *are* LVGL widget
  trees under Brookesia's app lifecycle, not raw framebuffer blits -
  Cadran's "render into a caller-owned pixel buffer, no engine, no
  LVGL" model doesn't fit that shape directly. Two real options, neither
  mechanical: render Cadran output into an off-screen buffer wrapped as
  an LVGL image widget (adds a translation layer, blunts some of
  Cadran's own cheapness), or give the watchface its own screen state
  outside Brookesia's normal app flow entirely (esp-watchos's AOD-
  toggle already shows precedent for exactly that kind of special
  state). Real design work either way. ~3-5 days.
- **`jw.*` native modules / HAL contract**: `board_hal/board.h`'s
  contract would need reimplementing against esp-watchos's own BSP
  layer (or that BSP wrapped to satisfy Kaliber's `board_desc_t`) -
  ~2-4 days, and this only ever covers the C6, because:
- **watchy_v3 doesn't exist in esp-watchos's world at all** - no board
  abstraction, one board, one firmware. Giving it a second, e-ink,
  deep-sleep board would hit the *exact same* LVGL-vs-deep-sleep
  problem Weg A has, from the esp-watchos side instead - Weg B doesn't
  avoid that problem, it just defers which codebase has to solve it
  first.
- **Manifest/signing/permission-capability model**
  (`package-signing.md`, `js-api.md`): pure logic and data format,
  fully portable regardless of which graphics stack sits underneath.
  ~1 day either way - a genuine wash between the two options.

**What's lost moving base**: `board_hal`'s two-board abstraction -
hardware-verified this session on a real deep-sleep e-ink board *and*
a real always-on AMOLED board, the specific thing display-regions.md
calls "the stress test for the HAL." esp-watchos has never had to pass
that test. Also at risk: Cadran's zero-engine-boot render path as a
real battery-life property on watchy_v3 specifically - LVGL/Brookesia's
whole execution model assumes continuous operation, which is in
tension with exactly what makes Cadran valuable there.

**What's gained**: a real, daily-driver-proven (Jan's own use,
`README.md`'s working app list, active git history) rendering, font,
icon, and gesture stack for the C6 - including mature tap/swipe/long-
press support in LVGL's own `indev` system, which would directly
replace the hand-rolled swipe classification just built today.

**Rough estimate**: 15-25 days (engine + store + signing port, Cadran-
vs-LVGL/Brookesia reconciliation, a from-scratch HAL contract) - and at
the end, watchy_v3's story is *not* solved, only deferred.

## 4. Memory, against the measured numbers

Kaliber's own C6 numbers (this session, hardware-measured): 364 KB free
after boot, QuickJS engine ~62 KB, WiFi ~54 KB, Cadran stripe buffer
26,240 B (32 lines × 410 × 2 B). Worst case, all concurrent:
`364 - 62 - 54 - 26 ≈ 222 KB` still free today.

esp-watchos's own tuned LVGL buffer: 32,800 B (40 lines × 410 × 2 B) -
**the exact same per-line cost as Kaliber's own stripe buffer**
(820 B/line either way; both are just width × 2 bytes, RGB565 is RGB565
regardless of which project computes it). A single LVGL buffer fits
numerically inside the 222 KB margin (`222 - 33 ≈ 189 KB` left) - raw
*pixel buffer* cost is not the real risk.

**The real cost LVGL adds isn't the buffer, it's the retained widget
tree** - every `lv_obj_t` plus its style/event/animation state carries
real per-widget overhead (LVGL's own docs put a bare object around
100+ bytes before any content), and Brookesia's app-lifecycle
bookkeeping sits on top of that again. Cadran's own model has
essentially none of this: a widget in `face.bin` costs a fixed 24
bytes on flash and nothing at runtime beyond the render pass itself -
no live object graph, no per-widget heap allocation, by design. A
handful of LVGL widgets is very likely fine inside the current margin;
a full Brookesia-style app switcher, status bar, and multi-screen
settings UI resident in RAM at once is the part worth measuring before
trusting a napkin number - not designed further here.

## 5. Recommendation

**Weg A, narrowed - not the full LVGL adoption Simon's framing poses as
the alternative to Weg B.** KaliberOS's actual, hardware-verified asset
is `board_hal` working identically across a deep-sleep e-ink board and
an always-on AMOLED board, plus Cadran's zero-engine-boot render path,
which specifically pays for itself on the board that needs it most.
Both are real, proven, and neither survives Weg B's move intact - Weg B
would rebuild the *harder* problem (one contract, two power models)
from scratch inside a codebase that has never had to face it, while
"only" reusing Weg B's font/icon/gesture maturity, which is the
*easier* problem and the one Kaliber's own Phase 4 was always going to
have to solve on its own terms anyway (an atelier-based resource
pipeline, not LVGL's, was already the plan before this comparison).

Concretely: don't adopt LVGL's widget system or Brookesia at all - pull
in only what's genuinely separable and hard to justify hand-rolling
twice, which today is narrower than "all of LVGL": **gesture
recognition** (LVGL's `indev` tap/swipe/long-press classification is
mature and battle-tested; the hand-rolled swipe code built today, while
working, is exactly the kind of thing worth not re-inventing if a clean
seam exists) is the strongest candidate, evaluated on its own as a
follow-up, not decided here. Fonts/icons stay Kaliber's own
atelier-pipeline plan (Phase 4, already scoped, doesn't need LVGL to
exist). Cadran, `board_hal`, the store, and the signing model stay
exactly as they are - they're the parts already proven across two
boards, and nothing in esp-watchos's own feature list (§1) answers a
question they don't already answer.
