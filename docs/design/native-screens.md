# Native bring-up screens — inventory

Every screen the launcher draws in C (no JS engine) today, in one place -
what's on it, at what scale/position, and when it appears. Written
2026-09-05 after the state machine's hardware bring-up (`launcher-states.md`
§1-2) made this worth doing once rather than five times ad hoc: each of
these screens was added by whoever needed it that day, at whatever scale
fit, with no shared reference. Not a replacement for
[`smartwatch-system/`](smartwatch-system/README.md) - that's the future
`jw.ui` widget-layer target look; this is the *current*, native-C,
bring-up state, kept legible until Cadran/the real menu (§3) replace each
entry.

Panel: watchy_v3, 200×200 monochrome e-ink (SSD1681). Font: built-in 8×8
bitmap (`gfx/text.h`), drawn at `scale` × that per glyph - so a line of N
characters is `N × 8 × scale` px wide, and must stay ≤ 200 to avoid
wrapping/clipping. That one constraint is why every screen below mixes
scales the way it does - see the hierarchy rule after the table.

## Screens

| Screen | Function | Trigger | Content (scale) |
|---|---|---|---|
| NO APPS | `draw_no_apps_screen()` (launcher.c) | WATCHFACE fallback, nothing installed | "NO APPS" (3) / "atelier push" (2) |
| MENU | `draw_menu_placeholder()` (launcher.c) | SELECT from WATCHFACE | "MENU" (3) / "SELECT: open" (1) / "BACK:   watchface" (1) / "DOWN:   install" (1) |
| Sync mode | `net_svc.c`'s `draw_sync_screen()` | DOWN from MENU (was: USB-connected on every boot, retired 2026-09-05) | "SYNC" (3) / SSID, PASS, IP (1 each - see below) |
| WAKE CHECK | `draw_wake_check_screen()` (launcher.c) | tick/button-wake-from-MENU, idle-from-APP (bring-up only, `docs/design/launcher-states.md` §1 invariants) | "WAKE CHECK" (2) / one-line result (1), shown ~1.5s then continues. Exempt from the hierarchy rule below - a temporary diagnostic aid, not part of the device's actual look, and "WAKE CHECK" itself doesn't fit scale 3 (10 chars, 240px) |
| hello (example) | `examples/complications/hello/app.js`, via the engine, not this launcher | WATCHFACE fallback (if installed) / APP | "KALIBER" (2) / "WAKES:" (1) / counter (6) |

## Hierarchy rule (resolved 2026-09-05, project chat)

One big headline (scale 3-4, whatever fits the panel width) states what
the screen *is*; at most one small detail block (scale 1-2) underneath
carries values worth reading closely. No line that doesn't fit its scale
unwrapped - if a phrase doesn't fit, the phrase is wrong, not the scale.
Long instructional text (full command lines, step-by-step) belongs in
the README, not the panel - a screen points, it doesn't teach.

Concretely, at this panel's 200px width: scale 3 (24px/glyph) fits ≤8
chars/line, scale 2 (16px/glyph) fits ≤12, scale 1 (8px/glyph) fits ≤25.
This is why sync mode's SSID/PASS/IP stayed at scale 1 rather than 2 -
`KaliberOS-CCA5` alone is 14 chars, already over scale 2's budget before
a "SSID: " label is even added; wrapping three already-short values
across more lines to gain readability nobody asked for wasn't worth it,
unlike NO-APPS/MENU/SYNC's headlines, which fit scale 3 outright.

## Known issue: e-ink ghosting between screens

Observed on hardware (2026-09-05, watchy_v3): switching from the `hello`
app's screen to MENU sometimes leaves the previous screen's text visible
under the new one, despite the framebuffer being `memset` to all-white
before drawing and `end_frame(true)` (full-refresh LUT, `board.c`'s
`disp_end_frame()`) being used. Not yet root-caused - candidates are the
SSD1681 full-refresh waveform itself, or `s_partials_since_full`/LUT
selection interacting badly with how often a "screen change" actually
requests `full=true`. Flagged here rather than guessed at; needs its own
investigation before any screen redesign work, since a wrong assumption
about *why* screens bleed into each other would make redesigning them
not actually fix what's wrong.
