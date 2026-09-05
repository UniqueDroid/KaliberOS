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
scales the way it does: scale 2 (16px/glyph, ≤12 chars/line) reads far
better than scale 1 (8px/glyph, ≤25 chars/line) but doesn't fit most of
the instructional hint lines as currently worded.

## Screens

| Screen | Function | Trigger | Content (scale) |
|---|---|---|---|
| NO APPS | `draw_no_apps_screen()` (launcher.c) | WATCHFACE fallback, nothing installed | "NO APPS" (2) / "atelier push to install" (1, 24 chars - doesn't fit at scale 2 unwrapped) |
| MENU | `draw_menu_placeholder()` (launcher.c) | SELECT from WATCHFACE | "MENU" (2) / "SELECT: open" (1) / "BACK:   watchface" (1) / "DOWN:   install" (1) |
| WAKE CHECK | `draw_wake_check_screen()` (launcher.c) | tick/button-wake-from-MENU, idle-from-APP (bring-up only, `docs/design/launcher-states.md` §1 invariants) | "WAKE CHECK" (2) / one-line result (1), shown ~1.5s then continues |
| Sync mode | `net_svc.c`'s own draw call | DOWN from MENU (was: USB-connected on every boot, retired 2026-09-05) | SSID/password/IP, own layout - not yet reconciled with this table's scale convention |
| hello (example) | `examples/complications/hello/app.js`, via the engine, not this launcher | WATCHFACE fallback (if installed) / APP | "KALIBER" (2) / "WAKES:" (1) / counter (6) |

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

## Open decision: hint-line scale

Filed here, not resolved: bumping every hint line (`"atelier push to
install"`, `"BACK:   watchface"`, etc.) to scale 2 doesn't fit this panel
at their current wording - each would need shortening or wrapping to two
lines. Candidates, not yet chosen: (a) shorten wording to fit ≤12 chars/
line at scale 2, (b) keep hints at scale 1 but improve contrast/spacing
instead, (c) wrap long hints across two scale-1 lines but bump to scale 2
where the wording already fits (`"BACK: face"` at 10 chars would). Needs
one decision, applied everywhere in the table above, not another
per-screen guess.
