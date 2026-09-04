# KaliberOS

A JS app framework for ESP32 watches and devices. The *Kaliber* is the
movement — firmware, RTOS base, and runtime. Apps are called
**Complications** (`.comp`), the engine layer is called **Unruh**, and
Complications are built with the **atelier**.

```
Complications (JS, ES5 baseline)      examples/complications/
────────────────────────────────
Launcher      App lifecycle, js_task  components/launcher/
Unruh         Engine abstraction      components/unruh/      (QuickJS | MQuickJS)
Core          Event bus, power, store components/core/
HAL           Board abstraction       components/board_hal/boards/<board>/
ESP-IDF / FreeRTOS
```

## Core ideas

- **One board = one directory.** `boards/<name>/` provides `board.c`
  (ops structs + capabilities) and `sdkconfig.defaults`. No board `#ifdef`
  outside the HAL. Core and the JS layer query `caps`, never board names.
- **One consumer, many producers.** `js_task` is the only place the engine
  API runs (QuickJS isn't thread-safe). ISRs and net_task only post events
  to the bus.
- **Two sleep models.** `caps.sleep_model_deep` chooses between Watchy-style
  (deep sleep, engine dies, state JSON survives in NVS/FS) and always-on
  (AMOLED boards, engine resident, light sleep).
- **Bytecode instead of source.** `atelier pack` compiles offline
  (qjsc / mqjsc); the device only ever loads bytecode. The manifest's ABI
  number must match the firmware (`KB_APP_ABI_VERSION`), or the launcher
  rejects it. Bytecode isn't verified → only load signed packages of your
  own (HMAC).
- **Permissions = existence.** Modules (`jw.net`, `jw.storage`, …) are only
  registered when both the manifest permission and the board capability
  allow it. What an app isn't allowed to do simply doesn't exist in its
  context.
- **Big buffers live on the heap, never on the stack.** Tasks here run on
  single-digit-KB stacks (main_task: 8 KB, the HTTP handler net_svc.c will
  use: ~4 KB by ESP-IDF default) while the JS engine alone budgets 96 KB of
  heap. A `uint8_t buf[10240]` local looks harmless in a diff but is bigger
  than most of this project's task stacks outright - it silently overflows
  into whatever's next in memory instead of erroring, and the resulting
  corruption can surface anywhere (a 2026-09-04 hang traced three call
  frames deep into littlefs turned out to be exactly this, one frame up in
  the caller). `malloc()`/`free()` it instead, always.

## Build

QuickJS-ng is vendored as a git submodule at `third_party/quickjs`, wrapped
as an ESP-IDF component by `components/quickjs/CMakeLists.txt` (which just
points `idf_component_register` at the submodule's core sources - the
submodule itself stays untouched upstream code).

```sh
git submodule update --init third_party/quickjs   # once, or clone with --recurse-submodules

idf.py -DKALIBER_BOARD=watchy_v3 set-target esp32s3
idf.py -DKALIBER_BOARD=watchy_v3 build flash monitor
```

## Building and pushing a Complication

```sh
export QJSC=/path/to/qjsc          # must match the firmware's QuickJS version!
tools/atelier/atelier.py pack examples/complications/hello --key <hexkey>
tools/atelier/atelier.py push de.jan.hello-0.1.0.comp --host <watch-ip> --port 8080
```

`atelier` is deliberately shaped to slot into an existing `release.sh`
pipeline (pack → push, exit codes, no interactivity).

`push` needs the watch to actually be listening: plug it into USB - that's
`net_svc.c`'s sync-mode trigger (see `board_hal`'s `usb_connected()`), no
button combo or menu yet - and the panel shows a SSID/password/IP screen
for `KALIBER_NET_SYNC_TIMEOUT_S` (120s default) or until the endpoint
gets a request, whichever comes first. `--key` must match the device's
own HMAC key (`get_hmac_key()` in `app_store.c` - generated into NVS on
first boot and logged once, or `KALIBER_STORE_HMAC_KEY_OVERRIDE` if
pre-provisioned).

## Design

[`docs/design/`](docs/design/) holds concept and architecture docs not implemented yet:

- a UI/UX concept for the future menu and Complication look-and-feel (watchface, list menus, health/notification/navigation/work-mode screens, design tokens) — reference for `jw.ui`'s widget layer.
- [Cadran](docs/design/cadran-watchface-engine.md), the planned `components/cadran/` declarative watchface renderer — minute ticks render in pure C from a serialized widget tree, no JS engine boot required. Slots in after the SSD1681 driver and font rasterizer, before MQuickJS.

## Status / Roadmap

Skeleton — architecture is in place, grunt work is flagged:

- [x] **Green `idf.py -DKALIBER_BOARD=watchy_v3 build`**, flashed and booted on a real v3 board (2026-09-01) - clean boot, `cadran_selftest()` passes on hardware, littlefs store mounts.
- [x] **Milestone 1 complete**: all four success criteria below hardware-verified (2026-09-01 to 2026-09-03).
- [x] **Cadran roadmap steps 1-4** (loader/renderer, provider registry,
      face.bin serializer + `WatchFace()`) hardware-verified 2026-09-03 -
      see [the design doc](docs/design/cadran-watchface-engine.md).
- [x] **Verify pins.h** (Watchy v3 schematic, PSRAM quad/octal!) - matches PicoWatch's production config.h exactly. PSRAM mode still needs a real check though, not covered by pins.h alone.
- [x] SSD1681 init sequence + blit/update in `boards/watchy_v3/board.c`, incl. a ghosting-mitigation full-refresh counter for partial updates.
- [x] Font rasterizer: `components/gfx` (8×8 bitmap font, ASCII 0x20-0x7e, scale param, per-pixel bounds check), wired into `jw.ui.text()`. No dependency on `unruh`, so `cadran`'s renderer can adopt it later.
- [ ] Timer wheel in `engine_quickjs.c` (`js_next_timer_ms` feeds the bus
      timeout — the mechanism is already wired up in the launcher)
- [x] `app_store`: untar, cJSON manifest, per-device HMAC check, atomic
      rename - hardware-verified 2026-09-04, incl. a real mkdir()/littlefs
      hang traced to a caller-side stack overflow (10 KB local on an 8 KB
      task stack), not app_store itself.
- [x] Sync endpoint (`net_svc.c`): WiFi-on-demand SoftAP + streamed
      `POST /install` - hardware-verified 2026-09-04, real end-to-end
      `atelier push` against the running watch, no reflash.
- [ ] MQuickJS backend (`engine_mqjs.c`) → unlocks v2/C6 boards
- [ ] Second board: `waveshare_c6_amoled` (MQuickJS, RGB565, always-on)
      as a stress test for the HAL

## Success criteria for the first milestone

1. [x] Log free heap after engine init on the target board (critical for
   v2). Real v3 numbers (2026-09-01, `hello` example, seeded via
   `main/seed_apps.c` - see its header comment): 192132 B free heap,
   engine using 62321 B of its 96 kB budget. **Note:** that only leaves
   ~34 kB for app objects on top of the engine's own baseline - fine for
   `hello`, tight for a real Complication. There's headroom in free heap
   to raise `js_heap_budget` (e.g. towards 128 kB), but don't just bump
   it: WiFi isn't wired up yet and its stack will eat 50-70 kB of that
   same free heap once it is. Re-measure with WiFi active before
   changing the budget.
2. [x] Wake→display latency < e-ink refresh time (engine boot must never be
   the bottleneck). Measured on real v3 hardware (2026-09-03), both paths,
   same panel:
   - E-ink full/partial refresh itself: ~1.4 s (`board.watchy_v3`'s
     `disp_update()`) - the actual bottleneck on both paths, unavoidable
     panel physics, not something either render path controls.
   - Engine path (`hello`): wake to render-ready (engine boot + bytecode
     load + hooks, before the blit) ~38 ms.
   - C-only path (Cadran, `js_watchface_selftest()`): `cadran_face_load()`
     + `cadran_render()`, no engine at all, ~0.76 ms - free heap 343 kB
     vs. 184 kB with the engine up, confirming the ~62 kB engine cost
     from criterion 1 lines up with a real, usable difference.
   Criterion holds for both paths (38 ms and 0.76 ms are both far under
   the 1.4 s panel refresh), but the C-path makes the engine's share of
   that budget disappear rather than just fit inside it.
3. [x] A `while(true)` Complication gets torn down by the budget handler
   after 500 ms as a JS error — without a watchdog reset. Verified on
   real v3 hardware (2026-09-01, `examples/complications/budget-hog`):
   aborted after ~511ms, clean `JS_ERR_TIMEOUT`, no crash. Found and
   fixed a real bug along the way - see the engine_quickjs.c commit from
   that date.
4. [x] Deep-sleep round trip: the counter in `hello` keeps counting across
   wakes. Verified on real v3 hardware (2026-09-03), on battery, 3
   consecutive cycles: wait 15s for idle-timeout sleep, press MENU, counter
   is exactly +1 each time. Was blocked by two bugs, both fixed first: a
   spurious GPIO0/ext1 wake loop (GPIO0 doubles as the USB auto-program
   strap pin) and the counter being unreadably small before the font
   rasterizer existed.
