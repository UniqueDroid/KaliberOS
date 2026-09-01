# Kaliber

Ein JS-App-Framework für ESP32-Uhren und -Geräte. Das *Kaliber* ist das Werk —
Firmware, RTOS-Basis und Runtime. Die Apps heißen **Complications** (`.comp`),
die Engine-Schicht heißt **Unruh**, gebaut werden Complications im **atelier**.

```
Complications (JS, ES5-Baseline)      examples/complications/
────────────────────────────────
Launcher      App-Lifecycle, js_task  components/launcher/
Unruh         Engine-Abstraktion      components/unruh/      (QuickJS | MQuickJS)
Core          Event-Bus, Power, Store components/core/
HAL           Board-Abstraktion       components/hal/boards/<board>/
ESP-IDF / FreeRTOS
```

## Kernideen

- **Ein Board = ein Verzeichnis.** `boards/<name>/` liefert `board.c`
  (ops-Structs + Capabilities) und `sdkconfig.defaults`. Kein Board-`#ifdef`
  außerhalb der HAL. Core und JS-Schicht fragen `caps` ab, nie Boardnamen.
- **Ein Konsument, viele Produzenten.** Die `js_task` ist der einzige Ort, an
  dem Engine-API läuft (QuickJS ist nicht threadsafe). ISRs und net_task
  posten nur Events in den Bus.
- **Zwei Schlafmodelle.** `caps.sleep_model_deep` wählt zwischen
  Watchy-Stil (Deep Sleep, Engine stirbt, State-JSON überlebt in NVS/FS)
  und Always-on (AMOLED-Boards, Engine resident, Light Sleep).
- **Bytecode statt Quelltext.** `atelier pack` kompiliert offline
  (qjsc / mqjsc), das Gerät lädt nur Bytecode. ABI-Nummer im Manifest muss
  zur Firmware passen (`KB_APP_ABI_VERSION`), sonst lehnt der Launcher ab.
  Bytecode wird nicht verifiziert → nur signierte eigene Pakete laden (HMAC).
- **Permissions = Existenz.** Module (`jw.net`, `jw.storage`, …) werden nur
  registriert, wenn Manifest-Permission und Board-Capability es erlauben.
  Was eine App nicht darf, existiert in ihrem Context schlicht nicht.

## Build

```sh
# QuickJS als Komponente bereitstellen (einmalig), z. B.:
git clone https://github.com/quickjs-ng/quickjs components/quickjs
#   -> braucht ein kleines idf_component_register-Wrapper-CMakeLists,
#      oder alternativ einen Registry-/Managed-Component-Port.

idf.py -DKALIBER_BOARD=watchy_v3 set-target esp32s3
idf.py -DKALIBER_BOARD=watchy_v3 build flash monitor
```

## Complication bauen und pushen

```sh
export QJSC=/pfad/zu/qjsc          # muss zur Firmware-QuickJS-Version passen!
tools/atelier/atelier.py pack examples/complications/hello --key <hexkey>
tools/atelier/atelier.py push de.jan.hello-0.1.0.comp --host <watch-ip>
```

`atelier` ist bewusst so geschnitten, dass es sich in eine bestehende
`release.sh`-Pipeline einhängen lässt (pack → push, Exit-Codes, keine
Interaktivität).

## Status / Roadmap

Skeleton — Architektur steht, Fleißarbeit markiert:

- [ ] **pins.h verifizieren** (Watchy-v3-Schaltplan, PSRAM quad/octal!)
- [ ] SSD1681-Init-Sequenz + blit/update in `boards/watchy_v3/board.c`
- [ ] Font-Rasterizer in `modules/js_ui.c` (erst mal 8×8-Bitmap-Font)
- [ ] Timer-Wheel in `engine_quickjs.c` (`js_next_timer_ms` speist den
      Bus-Timeout — Mechanik ist im Launcher schon verdrahtet)
- [ ] `app_store`: untar, cJSON-Manifest, HMAC-Prüfung, atomic rename
- [ ] Sync-Endpoint (`net_svc.c`): WiFi on demand + `POST /install`
- [ ] MQuickJS-Backend (`engine_mqjs.c`) → schaltet v2/C6-Boards frei
- [ ] Zweites Board: `waveshare_c6_amoled` (MQuickJS, RGB565, always-on)
      als Härtetest der HAL

## Erfolgskriterien für den ersten Meilenstein

1. Freier Heap nach Engine-Init auf dem Ziel-Board loggen (v2 kritisch).
2. Wake→Display-Latenz < E-Ink-Refreshzeit (Engine-Boot darf nie der
   Flaschenhals sein).
3. `while(true)`-Complication wird vom Budget-Handler nach 500 ms als
   JS-Fehler abgeräumt — ohne Watchdog-Reset.
4. Deep-Sleep-Roundtrip: Counter in `hello` zählt über Wakes hinweg weiter.
