# Kaliber

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
HAL           Board abstraction       components/hal/boards/<board>/
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

## Build

```sh
# Provide QuickJS as a component (once), e.g.:
git clone https://github.com/quickjs-ng/quickjs components/quickjs
#   -> needs a small idf_component_register wrapper CMakeLists,
#      or alternatively a registry/managed-component port.

idf.py -DKALIBER_BOARD=watchy_v3 set-target esp32s3
idf.py -DKALIBER_BOARD=watchy_v3 build flash monitor
```

## Building and pushing a Complication

```sh
export QJSC=/path/to/qjsc          # must match the firmware's QuickJS version!
tools/atelier/atelier.py pack examples/complications/hello --key <hexkey>
tools/atelier/atelier.py push de.jan.hello-0.1.0.comp --host <watch-ip>
```

`atelier` is deliberately shaped to slot into an existing `release.sh`
pipeline (pack → push, exit codes, no interactivity).

## Design

[`docs/design/`](docs/design/) holds a UI/UX concept for the future menu and Complication look-and-feel (watchface, list menus, health/notification/navigation/work-mode screens, design tokens). Reference for `jw.ui`'s widget layer, not yet implemented.

## Status / Roadmap

Skeleton — architecture is in place, grunt work is flagged:

- [ ] **Verify pins.h** (Watchy v3 schematic, PSRAM quad/octal!)
- [ ] SSD1681 init sequence + blit/update in `boards/watchy_v3/board.c`
- [ ] Font rasterizer in `modules/js_ui.c` (start with an 8×8 bitmap font)
- [ ] Timer wheel in `engine_quickjs.c` (`js_next_timer_ms` feeds the bus
      timeout — the mechanism is already wired up in the launcher)
- [ ] `app_store`: untar, cJSON manifest, HMAC check, atomic rename
- [ ] Sync endpoint (`net_svc.c`): WiFi on demand + `POST /install`
- [ ] MQuickJS backend (`engine_mqjs.c`) → unlocks v2/C6 boards
- [ ] Second board: `waveshare_c6_amoled` (MQuickJS, RGB565, always-on)
      as a stress test for the HAL

## Success criteria for the first milestone

1. Log free heap after engine init on the target board (critical for v2).
2. Wake→display latency < e-ink refresh time (engine boot must never be
   the bottleneck).
3. A `while(true)` Complication gets torn down by the budget handler after
   500 ms as a JS error — without a watchdog reset.
4. Deep-sleep round trip: the counter in `hello` keeps counting across wakes.
