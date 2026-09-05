# Design docs

Concept and architecture documents that aren't implemented yet, kept here for reference while the corresponding roadmap items land.

- [`smartwatch-system/`](smartwatch-system/README.md) — visual/UX concept for the future menu and Complication look-and-feel (watchface, list menus, domain screens, design tokens).
- [`cadran-watchface-engine.md`](cadran-watchface-engine.md) — architecture for `components/cadran/`, the declarative watchface renderer that skips booting the JS engine on a minute tick.
- [`display-regions.md`](display-regions.md) — striped rendering model (`display_ops_t`, `gfx`, `jw.ui`) for panels too large to fit a full framebuffer in RAM. Blocks the second board.
- [`launcher-states.md`](launcher-states.md) — watchface/menu/app state model above Cadran: what's active, persisted selection, the native menu, and the default out-of-box face. Priority: before the second board.
- [`native-screens.md`](native-screens.md) — inventory of the current native-C bring-up screens (NO APPS, MENU, WAKE CHECK, sync mode): what's on each, at what scale/position, and when it appears. Keeps ad hoc screen additions consistent until the real menu (§3) and Cadran replace them.
- [`js-api.md`](js-api.md) — the sensor/device module contract (`jw.sensors`/`jw.device`) a Complication's JS actually calls, Zepp OS-compatible where practical. Permission vs. capability, the manifest field for required modules, and the Watchface/App lifecycle split. First wave: Time, Battery, Step, Vibrator.
