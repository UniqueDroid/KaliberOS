# Display regions — striped rendering for large/RGB565 panels

Design doc for the region-based display model the second board (Waveshare
ESP32-C6-Touch-AMOLED-2.06) needs before any board code can be written.
Blocks `waveshare_c6_amoled` (README roadmap).

Status: design, no code yet. Target: a `caps.stripe_lines` field on
`board_desc_t`, a reshaped `display_ops_t`, and small adjustments to
`gfx`, `cadran`, and `jw.ui` - all backward compatible with the existing
single-buffer boards (Watchy v2/v3).

## 1. The problem, in numbers

C6-AMOLED: 410×502 panel, RGB565 (2 B/px) = 411,640 B ≈ 402 kB for a full
frame. The SoC has 512 kB SRAM total and no PSRAM. Measured costs from
this project's own hardware so far (Watchy v3, different SoC but the
only real numbers we have): WiFi resident ≈52 kB (`net_svc.c`'s sync-mode
summary log), JS engine ≈62 kB of its 96 kB budget (README success
criterion 1). 402 kB for one buffer plus ~114 kB for WiFi+engine already
exceeds 512 kB before touching anything else the system needs (heap
fragmentation headroom, task stacks, littlefs cache) - a full-panel
framebuffer is not a tight fit, it's arithmetically impossible on this
part.

Everything that currently assumes one full-panel buffer has to change:
`display_ops_t.blit(const uint8_t *fb, size_t len)`, `board_fb_size()`,
the launcher's single `L.fb = heap_caps_malloc(board_fb_size(), ...)`,
`cadran_render()`, `gfx_draw_text()`, and `jw.ui`'s primitives (all of
`components/unruh/modules/js_ui.c` writes into one `s_fb` at
board-absolute coordinates).

### 1a. Does the rest of the budget fit? (QuickJS vs. MQuickJS on the C6)

Worth answering before the port starts, not after: the ESP32-C6 is also
512 kB SRAM with no PSRAM - the *same memory class* as the ESP32-S3
Watchy runs on (`board.c`'s own `has_psram = false` comment). That means
the measured WiFi (~52 kB) and QuickJS baseline (~62 kB of a 96 kB
budget) numbers from §1 aren't just a same-order-of-magnitude guess for
the C6, they're from a chip with the same total SRAM budget - the
closest real data point available before a C6 board exists to measure
directly.

Summed against the stripe buffer this design settles on (§4, 26 kB at
`stripe_lines=32`): WiFi 52 kB + QuickJS baseline 62 kB + stripe 26 kB =
140 kB, against Watchy's own measured ~323 kB free heap at the
equivalent point in boot (`net_svc.c`'s "sync mode: entering" log,
before WiFi/engine are up) - leaving roughly 180 kB for actual app
objects, system/task-stack overhead, and littlefs buffers. That's *more*
headroom than Watchy's own README-documented number for the same
question (~34 kB left for app objects after its 62 kB engine baseline),
not less - the arithmetic favors QuickJS, not MQuickJS, on paper.

"On paper" is doing real work in that sentence, though: this reuses
Watchy's numbers as a proxy on a different SoC with its own ROM/system
reservations, cache configuration, and WiFi driver footprint (RISC-V vs.
Xtensa, different `esp_wifi` internals per target) - not a substitute
for a real measurement. Practical answer: **start the C6 board with
QuickJS** (the arithmetic supports it and it avoids maintaining a second
engine backend before it's proven necessary), but treat "log free heap
after `esp_wifi_init()` + engine init, on real C6 hardware, the same way
criterion 1 measured it on Watchy" as an early **go/no-go gate in the
board bring-up sequence (§9 step 3)** - before building out app-level
functionality on top, not after. If the real number doesn't hold up,
`engine_mqjs.c` (already on the roadmap, just not built yet) is the
documented fallback, not a redesign.

## 2. Design goal and the measuring stick

One rendering surface, striped into horizontal bands the C6 can hold in
SRAM; Watchy keeps rendering into one buffer that happens to be exactly
one stripe covering the whole panel. Concretely: **`caps.stripe_lines`,
0 meaning "one stripe = the whole panel"** - existing boards set nothing,
change nothing, and every call site that currently assumes a full-panel
buffer keeps doing exactly that, because for them it still is one.

The bar for everything below, stated explicitly because it's the thing
most likely to get bent under pressure while designing for the harder
board: **the Watchy code path must not get more complicated.** If a
change to `display_ops_t` or `gfx` makes `board.c`'s SSD1681 driver or
`cadran_selftest()` harder to read, the design is wrong, not the bar.

## 3. `display_ops_t`

```c
typedef struct {
    esp_err_t (*init)(void);
    /* Optional per-frame setup hook - most boards (including Watchy)
     * leave this NULL. A board that needs to open a write-window/
     * transaction spanning multiple blit_region() calls uses it. */
    esp_err_t (*begin_frame)(void);
    /* Blit one region: buf is w*h pixels, tightly packed in
     * board->caps.disp_kind's native format - NOT an offset into a
     * full-panel buffer. (x,y) are panel-absolute. */
    esp_err_t (*blit_region)(int x, int y, int w, int h, const uint8_t *buf);
    /* Push everything blitted since begin_frame() to glass. full=false
     * may use partial refresh (e-ink) or skip unchanged regions
     * (always-on RGB565, not implemented in v1 - see §7). */
    esp_err_t (*end_frame)(bool full);
    esp_err_t (*sleep)(void);
} display_ops_t;
```

`blit()`/`update()` are gone, replaced by `blit_region()`/`end_frame()`.
Watchy v3's `board.c` maps directly:

- `begin_frame`: `NULL` (nothing to set up - the SSD1681 RAM window is
  set fresh by every write already, see `ssd1681_set_ram_window()`).
- `blit_region(x, y, w, h, buf)`: the launcher calls this exactly once
  per frame with `x=0, y=0, w=DISP_W, h=DISP_H, buf=L.fb` (stripe_lines=0
  means one stripe covers the panel) - the body is today's `disp_blit()`
  unchanged, it never has to look at `x`/`y`/`w`/`h` at all since they're
  always the full panel.
- `end_frame(full)`: today's `disp_update(bool full)` unchanged, byte
  for byte, including the ghosting-mitigation counter
  (`SSD1681_FORCE_FULL_EVERY_N_PARTIALS`) - that logic already keys off
  "how many non-full updates happened," which is exactly "how many times
  was `end_frame(false)` called," not "how many regions." No change
  needed there beyond the rename.

Net effect on `board.c`: three functions rename (`disp_blit` and
`disp_update`'s signatures gain/lose parameters that are unused on this
board when `stripe_lines` is 0), otherwise unchanged. That's the
measuring stick from §2, met - confirmed by actually striping this exact
board (below), not just by argument.

A striped board's `begin_frame`/`blit_region`/`end_frame` depend on its
real controller (CO5300, out of scope - §8) but the shape is: accumulate
regions between `begin_frame()` and `end_frame()`, issue whatever the
controller needs per region (a column/row address-window command
sequence is the common case for QSPI/MIPI panel controllers), and
`end_frame()` is either a no-op (controllers that auto-commit per write)
or the actual "flip" command.

**A real bug this exact concern predicts, found on real Watchy hardware
(2026-09-04):** setting `stripe_lines=50` as a smoke test (4 real
regions, on a board whose display driver was already trusted, before any
new SoC/controller/framework path could be entangled with it on the
C6) found that `ssd1681_set_ram_window()` hardcoded the panel's Y
address range *and* Y address counter to 0 - every `blit_region()` call
after the first wrote to the same physical rows regardless of its real
`y`, so later stripes silently overwrote earlier ones instead of landing
at their own offset. `stripe_lines=0` never exercised this (the one
call's `y` is always 0, so the bug was invisible), which is exactly why
this smoke test - not just the design argument above - was worth
running before a second controller could confuse "region path is wrong"
with "new panel driver is wrong." Symptom was not a crash: a plausible-
looking but wrong image (stale content from a previous full-panel write
sitting in rows the new, buggy code never actually touched), the kind of
failure that "looks right" on a quick glance - verified past that with
per-region markers naming their own real origin, not just "the image
looks okay again." Fixed: the Y range (`0x45`) and Y counter (`0x4F`)
commands both take the region's real `y`/`h` now, not a hardcoded 0. Any
board's real display driver needs to make the same check - a region API
existing is not proof its address-window plumbing actually uses the
region it's given.

## 4. Stripe buffer ownership

`board_desc_t.caps` gains one field:

```c
uint16_t stripe_lines;  /* 0 = one stripe covers the whole panel */
```

`board_fb_size()` keeps its existing meaning - **the size of the buffer
a caller allocates and renders into** - but that's now the stripe size,
not necessarily the panel size:

```c
size_t board_fb_size(void) {
    const board_desc_t *b = board_get();
    uint16_t lines = b->caps.stripe_lines ? b->caps.stripe_lines : b->caps.disp_h;
    return pixel_bytes(b->caps.disp_kind, b->caps.disp_w, lines);
}
```

Every existing caller (`cadran_selftest()`, `net_svc.c`'s
`draw_sync_screen()`, `jw_ui`'s `ui_clear()`, the launcher's
`L.fb = heap_caps_malloc(board_fb_size(), ...)`) is unchanged source -
they already treat "the buffer I draw into" as an opaque size from this
function, which is exactly what stays true. Watchy's `stripe_lines=0`
makes this identical to today's value; nothing about non-striped boards
changes.

C6: `stripe_lines = 32` → `board_fb_size()` = 410 × 32 × 2 = 26,240 B
≈ 25.6 kB - matches the back-of-envelope number in the task. Against the
measured Watchy costs (WiFi ≈52 kB, engine ≈62 kB) plus whatever the C6's
own numbers turn out to be, 26 kB is comfortably affordable; the exact
`stripe_lines` value is a tuning knob, not an architectural choice - more
lines per stripe trades memory for fewer `onRender()`/`cadran_render()`
invocations per frame (§6).

Allocation stays exactly where it is today: the launcher, once, sized by
`board_fb_size()`. No new ownership question - the field moves from
"the whole panel, always" to "however much this board says fits," and
every allocator already asks the board via that one function.

## 5. `gfx` and Cadran: a draw context instead of a board pointer

`gfx_draw_text()` and the private `set_px()` (duplicated today in both
`components/gfx/text.c` and `components/cadran/render.c`, per their own
"kept as its own static copy" comments) currently take `const
board_desc_t *board` and bounds-check against `board->caps.disp_w/h`
directly, assuming `fb` spans the whole panel. Both need a small context
carrying the current stripe's offset and extent instead:

```c
typedef struct {
    uint8_t             *fb;       /* the current stripe's buffer         */
    const board_desc_t  *board;    /* unchanged - disp_kind, disp_w, ...  */
    int                  origin_y; /* panel-absolute y of fb's row 0      */
    int                  height;   /* fb's height in px (<= disp_h)       */
} gfx_ctx_t;

void gfx_draw_text(const gfx_ctx_t *ctx, int x, int y, const char *str, int scale);
```

`x`/`y` **stay panel-absolute** - callers (Cadran widgets, `jw.ui`
primitives) don't change their coordinates at all, the same mental model
as today. Internally, `set_px(ctx, x, y)` computes `local_y = y -
ctx->origin_y` and bounds-checks `0 <= x < disp_w` and `0 <= local_y <
ctx->height` before writing - a pixel outside the current stripe is
silently clipped by the exact same mechanism that already clips pixels
outside the panel edge today (that check already exists in both copies
of `set_px()`; this only widens what "outside" means from "past the
panel" to "past the panel, or past this stripe"). For Watchy
(`origin_y=0, height=disp_h`), the arithmetic is identical to today's -
zero behavioral change, zero extra cost beyond one more struct-member
read per pixel.

This directly answers **widgets/draw calls spanning a stripe boundary
(task question 3)**: they're clipped per stripe, not skipped. A `rect`
widget straddling two stripes draws whatever fraction falls in each
stripe as that stripe renders - no special-casing, because the clip
check doesn't know or care that it's a "boundary" case any more than it
knows a widget is near the panel's own edge today.

`cadran_render()` gains an origin/height pair (or takes a `gfx_ctx_t`
directly, mirroring §5's shape):

```c
esp_err_t cadran_render(const cadran_face_t *face, const gfx_ctx_t *ctx);
```

Per-widget cost stays "iterate the flat widget list once per stripe" -
the task's own framing ("das sollte gut passen, weil die Widget-Liste
ohnehin flach und billig zu durchlaufen ist") holds: a widget whose
bounding box doesn't overlap `[origin_y, origin_y+height)` at all can be
skipped before touching a single pixel (a cheap y-range check against
the widget's own y + its type's rough extent), and widgets that do
overlap pay only for the pixels the per-pixel clip lets through. For
Watchy (one stripe, `n_stripes=1`), this is exactly today's single
`cadran_render()` call.

The flat-list traversal being cheap doesn't extend to what a widget
*does* once it overlaps a stripe: `rect`/`line`/`hand`/`text` are pure
compute today, but `img`/`img_digits`/`arc`/`img_level` (currently
parsed-but-skipped, pending the atelier resource pipeline - roadmap
§9 step 5 of the Cadran doc) will read pixel data from flash/littlefs.
Loading a whole image asset fresh on every stripe that overlaps it (up
to 16× on the C6) is real, avoidable I/O cost. When that pipeline lands,
resource widgets need to read only the row range `[origin_y,
origin_y+height)` of their own asset per stripe (a seek + bounded read,
the same "never buffer more than the current stripe" discipline
`net_svc.c`/`app_store.c` already use for install-time I/O - see their
module comments), not decode/load the whole image once per stripe. Not
this doc's problem to solve (the resource pipeline doesn't exist yet),
but worth flagging now so whoever builds it doesn't have to rediscover
the constraint.

## 6. `jw.ui` / imperative apps

This is the real design question, not a mechanical follow-on from §5 -
worth stating the tension plainly before picking a side.

**Option A - re-invoke `onRender()` once per stripe.** The launcher's
`app_render_if_dirty()` loops over stripes, rebinds `jw.ui`'s framebuffer
to a fresh `gfx_ctx_t` each time (`jw_ui_bind_fb()` gains `origin_y`/
`height` parameters), and calls `JS_HOOK_ON_RENDER` again per iteration.
App code is **unchanged** - `jw.ui.text(20, 300, ...)` still means panel-
absolute (20, 300), clipped transparently by whichever stripe is current
(§5's mechanism, shared between Cadran and `jw.ui`). The cost: on the C6
at `stripe_lines=32`, that's `ceil(502/32) = 16` invocations per frame
instead of 1, and **`onRender()` must be idempotent** - a pure function
of app state, called N times, producing the same pixels for the same
input every time.

**Option B - a smaller logical canvas.** Imperative apps get a reduced
resolution (or reduced bit depth) canvas that fits in one buffer, which
the C layer then upscales/tiles onto the real panel. `onRender()` stays
a single call, as today.

**Recommendation: Option A.** Three reasons, in order of how much they'd
change if the numbers came out differently:

1. **It's already close to true for every app in this repo.**
   `examples/complications/hello/app.js` and `budget-hog/app.js` both
   only *read* `this.count` in `onRender()` - state mutation happens in
   `onEvent`/`onSuspend`. An `onRender()` written the way this project's
   own examples already are pays nothing extra under Option A: it's
   idempotent by construction, N calls draw the same thing N times.
   Enforcing idempotence isn't a new constraint invented for one board,
   it's naming a convention that was already the honest way to write
   `onRender()` even on Watchy - Cadran's `build()` (design doc §1) is
   the same idea taken further, pure-by-construction because it only
   ever runs once. Option A pulls imperative apps one step toward that
   same discipline instead of adding a second, incompatible rendering
   contract next to it.
2. **Option B doesn't actually solve the memory problem, it moves it.**
   A canvas small enough to fit in one buffer *and* stay at the panel's
   real pixel density is a contradiction - 410×502 RGB565 is the 402 kB
   number from §1 regardless of who allocates it or when. A canvas that
   fits by being genuinely smaller (say 128×160) means every imperative
   app looks visibly worse on the nicer display than on Watchy, which
   undercuts the point of shipping a nicer display at all. Option B is
   only cheap if it quietly breaks the "same app runs everywhere"
   promise in a way users can see; Option A breaks a promise
   ("`onRender()` runs once") that was never really documented as a
   guarantee and that well-written apps don't rely on anyway.
3. **The failure mode is loud, not silent.** An app that mutates state
   in `onRender()` under Option A doesn't corrupt anything - it
   over-counts or flickers, visibly, immediately, on the one board where
   it matters. That's a bug an app author finds in the first five
   minutes of testing on hardware, not a subtle portability trap.

Cost estimate: no hard numbers exist for `onRender()` call overhead in
isolation yet (only the C-path Cadran numbers from README's success
criterion 2: 0.76 ms without engine, vs. 38 ms wake-to-render with).
16 re-invocations of an already-warm JS hook, each doing a handful of
`jw.ui` calls, is very unlikely to approach e-ink's 1.4 s refresh
budget - but this is a real open number to actually measure once a C6
prototype exists, not something to assume from this doc.

**The time budget must be tracked per render pass, not per call.**
`js_call_hook()` (`components/unruh/engine_quickjs.c`) gives every call
its own fresh `hook_budget_ms` deadline (`e->hook_deadline_us =
esp_timer_get_time() + hook_budget_ms * 1000`, reset at the top of every
call) - correct for a single `onRender()` invocation, but naively
calling it once per stripe means a hung app gets `hook_budget_ms` **per
stripe**, not per frame: 16 stripes × the existing 500 ms default
(README success criterion 3's measured value) is an 8 s worst case
before the launcher gives up, not the half-second an app author or a
device owner would reasonably expect. `js_call_hook()`'s per-call
deadline stays exactly as it is (still a real backstop if one single
stripe's call hangs forever) - the launcher's stripe loop adds an outer
ceiling on top: track wall-clock time from the *first* stripe's
`onRender()` call, and if the cumulative elapsed time across the calls
made so far exceeds `hook_budget_ms`, stop issuing further stripes for
this frame and fail the app the same way a single over-budget hook does
today (`app_fail("render")`) - a partially-rendered frame from an app
that was already over budget is an acceptable, already-failing outcome,
an 8 s hang is not. For Watchy (one stripe), this outer ceiling and the
existing per-call one are the same check, so nothing changes there.

**Escape hatch:** a manifest field (`"render": "single-buffer"`) lets an
app opt out and get one call against a full-panel buffer, on boards
where that fits - explicit trade-off the app author accepts, not a
silent fallback, and unavailable on boards where the arithmetic in §1
makes it impossible outright (the C6, checked at install time against
`caps.stripe_lines` and free heap). Not required for v1; noted so the
constraint in this section reads as "the default, with a documented
exit" rather than "the only option."

## 7. Dirty-region tracking (design now, build later)

`end_frame(bool full)`'s `full` parameter is an e-ink concept - it
means "use the full-refresh LUT, not the partial one" (§3), a real
hardware distinction that only exists because e-ink ghosts. AMOLED
doesn't ghost and has no such LUT choice to make; a striped AMOLED
board's `end_frame()` implementation simply ignores `full` (§3 already
says this). But that doesn't mean full-panel redraws are free there
instead - the consequence just moves from "visible ghosting" to
"invisible cost, same-shaped problem": redrawing a stripe means writing
its pixels over the AMOLED interface, and *lighting* those pixels -
bandwidth and power that scale with what's actually redrawn, not with
whether the panel would visibly ghost otherwise. On the C6, where the
whole reason this design exists is a tight power/memory budget, a
render loop that always redraws every stripe leaves real savings on the
table for no protocol reason.

**Whether a redraw can be skipped depends on who's rendering:**

- **Cadran can do this.** A declarative/hybrid face's widget list and
  the provider values they're bound to are directly comparable tick to
  tick - a widget's bound value either changed since the last render or
  it didn't (the loader already resolves `bind_id` to a value every
  render, see `cadran_render()`). Dirty-region tracking here means: diff
  which widgets changed, union their bounding boxes into the set of
  stripes actually touched, and only call `blit_region()` for those -
  skip the rest of the loop's iterations entirely, don't render into
  them at all.
- **Imperative `onRender()` apps cannot.** `jw.ui` has no visibility
  into what an app's pixels *mean* - `ui_text(x, y, " stopwatch: 47", …)`
  called twice with different strings looks the same to `jw.ui` as
  called twice with the same string, and there is no general way to
  diff "the app drew something" against "the app drew the same thing
  again" without re-rendering it first. These stay conservative: any
  dirty stripe from `jw_ui_take_dirty()` (per §6, unchanged) means every
  stripe in that render pass gets blitted, exactly like today.

**Why this doesn't need the render loop redesigned when it's built:**
the loop in §6 already iterates stripes one at a time, calling
`blit_region()` per stripe inside the loop body - dirty-region tracking
is a **filter on which iterations reach that call**, not a different
loop shape. For Cadran specifically: skip the whole render+blit for any
stripe whose widget diff came up empty. `begin_frame()`/`end_frame()`
still bracket the *pass*, not each stripe, so a pass that skips most
stripes still opens and closes cleanly. Nothing in §3-§6 needs to change
to add this later - it's what "leaves room for it" (previously in this
section as a §7 bullet, now expanded here per review) concretely means.

## 8. Not in this design

- CO5300 driver, touch, PMIC, RTC - real `boards/waveshare_c6_amoled/`
  work, after this doc is agreed. The official BSP in the component
  registry (`waveshare/esp32_c6_touch_amoled_2_06`) is a reference for
  that step, not for this one.
- Actually building §7's dirty-region tracking - designed above, not
  implemented; v1 redraws every stripe every dirty frame regardless of
  board, same as e-ink's full/partial distinction already existing
  independently of this design.
- MQuickJS - orthogonal to this doc; whichever engine calls
  `JS_HOOK_ON_RENDER` doesn't change how many times or into what buffer
  it gets called.

## 9. Roadmap placement

Blocks `boards/waveshare_c6_amoled/` (README roadmap: "second board...
stress test for the HAL"). Suggested order once this doc is agreed:

1. [x] `caps.stripe_lines` + `display_ops_t` reshape, Watchy v3 ported to
   the new shape with `stripe_lines=0` (§2's bar: verify on real
   hardware that nothing about its behavior changed - same e-ink
   selftest, same ghosting counter, before this is trusted). Done and
   hardware-verified 2026-09-04.
2. [x] `gfx_ctx_t` + Cadran per-stripe rendering, `jw.ui` per-stripe
   rebinding in the launcher, render-pass budget ceiling in the launcher
   (§6). Done and hardware-verified 2026-09-04, including a
   `stripe_lines=50` smoke test on this same board (§3's callout) that
   found and fixed a real bug in the SSD1681 driver's RAM-window
   addressing - exactly the kind of thing worth catching before a second
   controller is also new. Confirmed `stripe_lines=0` unaffected before
   reverting to it (the real Watchy value).
3. [x] `boards/waveshare_c6_amoled/`: display driver (SH8601/CO5300,
   QSPI) + boot, `stripe_lines=32` as planned, hardware-verified
   2026-09-05 - touch/PMIC/RTC still open (this board has no physical
   buttons at all, confirmed on hardware; touch is the only input path,
   deliberately deferred). **Go/no-go gate: PASSED.** Measured on real
   hardware, same shape criterion 1 used on watchy_v3: free heap after
   boot ~364 KB, after QuickJS engine init ~62 KB used (matches
   watchy_v3's number closely - same engine, same ABI), WiFi cost 53,648
   B entering→AP-up (vs. this doc's §1 estimate of ~52 KB - close), one
   framebuffer stripe (410×32×2) 26,240 B exactly as predicted. QuickJS
   fits comfortably; no `engine_mqjs.c` fallback needed on this board.
4. [x] The actual hardware test this was always for: does the same
   Complication - unmodified - render correctly on both boards. Yes,
   with one fix: the default watchface (docs/design/launcher-states.md
   §4) originally hardcoded x/y for watchy_v3's 200×200 panel and
   rendered off-center on the C6's 410×502 one - corrected to center
   relative to `ctx.w`/`ctx.h` (already passed into `build()`, Cadran
   doc §3) instead of adding board-specific coordinates. Same firmware,
   same Complication, two very different boards, both correct - this is
   what "the HAL actually carries the abstraction" looks like verified,
   not assumed.
