# Smartwatch System — design concept

Source: a [Claude Design](https://claude.ai/design) handoff bundle (2026-09-01), stored as-is under `smartwatch-system/` for reference. Open `smartwatch-system/Smartwatch System.dc.html` to browse the original mockups.

This is a **concept and reference for Kaliber's future menu/UI layer**, not a spec to implement pixel-for-pixel right now. `jw.ui` (`components/unruh/modules/js_ui.c`) currently only exposes raw primitives (`clear`, `text`, `invalidate`) — no list, grid, or component system exists yet to render this design as-is. Treat it as the target look-and-feel to build `jw.ui`'s widget layer towards, once the font rasterizer and layout primitives from the main [README](../../README.md#status--roadmap) roadmap land.

The mockups assume a 390×450 px rectangular display — this doesn't match any board in the current roadmap yet (`watchy_v3` is e-ink at a different resolution; the planned `waveshare_c6_amoled` second board's exact panel is still open). Don't read the 390×450 number as a hardware decision — it's the placeholder canvas the design tool used.

## Design language ("Modernist · Wearable")

- Flat, high-contrast: pure black/white + a single red accent (`#ec3013`), used as a filled area for exactly **one** focus element per screen — never as decorative color.
- No corner radius anywhere, 2px hairline dividers everywhere (same stroke weight as icons).
- Type: Archivo, weight 700/800 for anything a user reads as data or acts on. Time-of-day (132px) is the single largest element in the whole system.
- Icons: Lucide, 24px, 2px stroke, outline-only (never filled except in the active/focus state).
- 8px base grid, 24px side margin (broken only by full-bleed rows/action bars), 66px list row height, 44px minimum hit target, 68–72px action bar height.

## Screens covered

- **Watchface** — day (light) and always-on ambient (inverted, dimmed accent) variants.
- **Menu** — top-level list, scrolled state with a highlighted row, and a health submenu (label + value rows, no icons/chevron).
- **Four domain screens** — one representative screen per domain: health (heart-rate detail with 24h bar chart), notification (incoming, two-action bar), navigation (turn-by-turn), and "work mode" (shift timer + toggles for mute/glove mode/emergency button). The work-mode screen suggests this concept leans industrial/workwear rather than general consumer fitness.
- **Foundations** — color roles, type scale, icon set, reusable components (list row, action bar, switch), and the base grid/hit-area reference.

## How to use this

When building out `jw.ui`'s widget layer (list rows, focus/selection state, action bars) or a watchface Complication, check this doc's measurements and states first instead of inventing new ones — keeps every Complication visually consistent without a shared component library baked into the engine.
