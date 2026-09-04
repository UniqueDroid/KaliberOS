# Design docs

Concept and architecture documents that aren't implemented yet, kept here for reference while the corresponding roadmap items land.

- [`smartwatch-system/`](smartwatch-system/README.md) — visual/UX concept for the future menu and Complication look-and-feel (watchface, list menus, domain screens, design tokens).
- [`cadran-watchface-engine.md`](cadran-watchface-engine.md) — architecture for `components/cadran/`, the declarative watchface renderer that skips booting the JS engine on a minute tick.
- [`display-regions.md`](display-regions.md) — striped rendering model (`display_ops_t`, `gfx`, `jw.ui`) for panels too large to fit a full framebuffer in RAM. Blocks the second board.
- [`launcher-states.md`](launcher-states.md) — watchface/menu/app state model above Cadran: what's active, persisted selection, the native menu, and the default out-of-box face. Priority: before the second board.
