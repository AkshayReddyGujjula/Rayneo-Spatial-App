# R3-B — controller UI visual QA

Round 3, 2026-11-13. A QA pass over the offscreen renders of the redesigned controller (Track C),
fixing only clear defects. No layout restructuring, no new features, no behaviour or engine/IMU
change. Method: build (MSVC RelWithDebInfo, `/W4 /permissive-`), render all 27 `ui_snapshot`
frames, look at every PNG, compute WCAG contrast of the token pairs in `ui_theme.cpp`, fix, render
again and compare. Nothing ran against a display or the glasses.

## Fixed

| # | Defect (where seen) | Fix | Commit subject |
|---|---|---|---|
| 1 | Contrast below WCAG AA (table below): white label on the dark primary button 3.67:1 (2.83:1 hovered); `text_faint` captions/hints/step labels 3.16-4.36:1 in both themes; light `ok`/`warning`/`error` status text and badge labels 3.08-4.35:1; dark `accent` on its badge ("triple") 3.69:1; `accent` step number on `accent_soft` 3.84 / 4.28:1 | New `accent_fill` token for the primary button fill (the accent was doing double duty as text colour and fill); `text_faint`, dark `accent`/`error`, light `accent`/`ok`/`warning`/`error` moved just far enough along their own hue to pass. `ui_snapshot` now prints the contrast table and fails on any pair below AA (it reports the failures above against the old values) | UI QA: bring theme token pairs to WCAG AA and check them in ui_snapshot |
| 2 | Home > Readiness: with an action button the fix-it detail was cut mid-word ("press Win+P and choo...", "once on the Tracki...") at 1280x800 | Detail wraps, the row grows by the extra lines; one-line rows keep 64 px | UI QA: wrap Readiness fix-it details instead of cutting them off |
| 3 | Layout > Arrangement 2D: "left -45°" / "right 45°" labels sat on the angled screen lines (all sizes, both themes) | Label placed along the screen's outward normal, clear of line and selection halo; inward or pushed-inside fallbacks | UI QA: keep arrangement editor labels off the screens and the hint uncut |
| 4 | Same editor at 820x640: once labels move, the collision step could push the selected label out of the canvas (label vanished) | Collision stepping never leaves the canvas; a pushed-inside position is preferred to a clipped one | (same commit) |
| 5 | Arrangement 3D: labels crossed the outline of foreshortened screens ("eft -4\|5°" at 150 %) | Soft canvas-coloured plate behind each label | (same commit) |
| 6 | Arrangement hint ellipsized at every non-fullscreen size ("...up or dow...", "...drag to move it. ...") and the summary truncated at 820 px | When hint + summary do not fit on one line, the hint wraps full width and the summary gets its own line | (same commit) |
| 7 | Setup guide stepper: "Welcome" and "Done" ~40 px off their circles (fixed 120 px label boxes clamped at the edges) | Labels measured, centred under the circle, stopped at the stepper edge | UI QA: align setup-guide step labels with their circles; match button wording |
| 8 | Setup guide, head calibration: status says "Keep it with Next" while the button reads "Keep and continue" | "Keep it and continue, or redo it ..." | (same commit) |

## Left as taste (not changed)

* **Single-setting cards use the row style** (Comfort > Night tint, Settings > Keep running /
  Setup guide use a 10.5 pt semibold row title, other cards a 14 pt card title). Consistent
  pattern: every card whose whole content is one switch/button row is drawn as that row.
* **Units: "deg" vs "°"** — inspector/sliders/summary say `0 deg`, the editor canvas says `0°`.
  Changing the field formatter touches `format_layout_field_value` (app model, selftested) and the
  summary text; worth one consistent decision by the owner, not a QA fix.
* **Engine status strings in lowercase with pipes** ("running | workspace | 3 screens | 60 fps |
  VDD desktops", "3 screens | arc -45..45 deg | 46 deg fov | 120/30/1 fps") and raw calibration
  metric names (`up_vs_yaw 0.84 deg, nod_level ...`): wording of model/tool output, not paint code.
* **Two-space separator before parentheticals** ("...not found  (open the engine log ...)",
  "Running  (pid 4242)"): used consistently across the app as a house separator.
* **Arrangement title hidden at narrow widths**: deliberate ("the title yields to the toolbar").
* **Wizard compass "Earbuds in" row capped at 620 px**, so its switch is mid-dialog: deliberate
  cap that keeps the switch near its label.
* **Gap between stepper labels and the step title** (~17 px at 100 %) is tighter than the 40 px
  dialog padding; reads fine, layout constant.
* **Component boundaries** (`border`, `border_strong`, unfilled slider track) are 1.0-1.9:1. Cards
  and secondary buttons are identified by their text, and every stateful control carries a
  >= 3:1 indicator (switch thumb `text_dim`/white, slider knob and fill `accent`, focus ring
  >= 4.78:1), so these were left decorative rather than darkening every outline.
* Disabled text (`disabled_text`) is exempt from WCAG contrast and was not changed.

## Contrast (final, printed by `ui_snapshot`)

AA: 4.5:1 for text (no UI text is large enough for the 3:1 large-text rule apart from titles in
`text`), 3:1 for the focus ring. Badge = colour as text on `mix(surface, colour, 0.18 dark / 0.12
light)`, the tint `draw_badge` uses.

| Pair | Dark before | Dark after | Light before | Light after |
|---|---:|---:|---:|---:|
| text on window / surface_alt | 16.28 / 13.44 | same | 15.96 / 15.25 | same |
| text_dim on surface / surface_alt | 7.39 / 6.65 | same | 7.17 / 6.33 | same |
| text_faint on window | 4.19 | 5.58 | 3.40 | 4.84 |
| text_faint on sidebar | 4.36 | 5.80 | 3.16 | 4.51 |
| text_faint on surface | 3.85 | 5.12 | 3.68 | 5.24 |
| text_faint on surface_alt | 3.46 | 4.61 | 3.25 | 4.63 |
| accent on surface | 4.64 | 6.00 | 5.17 | 5.70 |
| accent on accent_soft (step number) | 3.84 | 4.96 | 4.28 | 4.72 |
| accent on its badge | 3.69 | 4.51 | 4.37 | 4.80 |
| white on primary fill | 3.67 | 5.25 | 5.17 | 5.70 |
| white on primary hover | 2.83 | 4.58 | 6.46 | 6.46 |
| ok on window / surface | 8.52 / 7.82 | same | 3.50 / 3.79 | 4.97 / 5.38 |
| ok on its badge | 5.56 | same | 3.28 | 4.55 |
| warning on window / surface | 9.86 / 9.05 | same | 3.25 / 3.51 | 4.98 / 5.39 |
| warning on its badge | 6.21 | same | 3.08 | 4.58 |
| error on window / surface | 5.87 / 5.38 | 6.41 / 5.88 | 4.35 / 4.70 | 5.00 / 5.41 |
| error on its badge | 4.21 | 4.50 | 3.95 | 4.53 |
| focus ring on surface | 8.52 | same | 5.17 | same |

Token changes (`ui_theme.cpp`): dark `text_faint` 112,120,137 -> 134,141,156; dark `accent`
66,129,247 -> 100,152,248; dark `accent_fill` (new) 52,104,204; dark `accent_hover` 96,152,255 ->
58,114,217; dark `error` 240,98,98 -> 241,111,111; light `text_faint` 124,134,151 -> 100,109,122;
light `accent` (and `accent_fill`) 37,99,235 -> 33,93,222; light `ok` 22,150,90 -> 18,122,73;
light `warning` 190,122,6 -> 148,95,5; light `error` 212,58,58 -> 195,53,53. Visible effect: the
dark primary button is a deeper blue, dark accents slightly lighter, light status colours a
little deeper; the hierarchy text > text_dim > text_faint is kept in both themes.

## Before / after (`docs/overnight/ui-qa/`, crops of the `ui_snapshot` frames)

* `readiness-light-before.png` / `-after.png` — Home Readiness, 1280x800 light: truncated fixes; light status colours.
* `arrangement-2d-before.png` / `-after.png` — Layout editor 1280x800 dark: labels on lines, cut hint.
* `arrangement-3d-150-before.png` / `-after.png` — 3D editor 1920x1080 @150 %: labels crossing outlines.
* `stepper-light-before.png` / `-after.png` — setup guide stepper: "Welcome"/"Done" alignment.

## Verification

All 27 frames re-rendered and inspected after the fixes; hotspot checks (inside the window, no
overlaps, help topics) PASS, contrast check PASS. 11/11 ctest, no new warnings. Not verified:
the live window (hover/press colours of the new `accent_fill` are rendered only at rest), and
ClearType on a real light-theme panel.

## Wording pass (R4-B)

Follow-up to the "left as taste" items on status strings, calibration metric names and units.
Only text shown on normal pages changed. The technical forms stay where they are diagnostic:
the Diagnostics page Engine row and Layout file row, the engine log, the status file, the tool's
stdout and its `@@progress` lines are unchanged. Separator is the Readiness rows' `  ·  `.

New app-model formatters (each with `app_selftest` cases): `engine_status_display_summary`,
`layout_display_summary`, `format_degrees`, `cal_user_units`, `calibration_saved_text`.
`engine_status_summary` and `layout_summary` are unchanged and now feed only the Diagnostics page.

| Where | Before | After |
|---|---|---|
| Home hero detail (engine running) | `running \| workspace \| 3 screens \| 60 fps \| VDD desktops` | `Workspace · 3 screens · 60 fps · virtual desktops` |
| Same, other parts | `\| VDD unavailable`, `\| laptop display detached`, `\| no head tracking`, `\| monitor 1 1920x1080` | `virtual desktops unavailable`, `laptop screen off`, `no head tracking`; monitor index/size dropped (Diagnostics keeps it) |
| Same, starting / failed / no status | `starting \| preview ...`, `failed \| <error>`, `no status yet` | `Preview · starting · ...`, `Stopped with an error: <error>`, `Waiting for the engine to report its status` |
| Readiness rows: Engine detail (Home) | technical summary | display summary; Diagnostics shows the technical one via `HealthItem::raw` |
| Home Layout card and Layout page summary | `3 screens \| arc -45..45 deg \| 46 deg fov \| 120/30/1 fps` | `3 screens · arc −45° to 45° · 46° field of view · 120/30/1 fps capture` (one screen: `1 screen · at 0° · ...`; empty: `No screens`) |
| Layout inspector / quick sliders (yaw, pitch, roll, FOV, enter, leave) | `-45 deg`, `0 deg` | `−45°`, `0°` (U+2212 minus, rounded whole degrees, never `−0°`) |
| Calibration saved panel (Tracking page and setup guide), head | tool detail, e.g. `up_vs_yaw 6.10 deg, nod_level 5.40 deg, nod_tilt 12.80 deg, handedness 9.95 deg` | by band: Good `The steps agreed closely, so your head axes are straight. The new calibration is in use from the next start.` / Fair `The steps agreed reasonably well. The new calibration is in use from the next start; if the screens tilt when you nod, run it again.` / Retry `The steps disagreed with each other. The calibration was saved, but running it again should give a straighter result.` / no score `The new calibration is in use from the next start.`; plus `Next, calibrate the compass for drift-free heading.` when the tool says so |
| Calibration saved panel, compass | `field 51.5 uT, residual 0.80 uT, turn 360 deg, tilt 40 deg, observability 0.120[; for a better result repeat and ...]` | `The heading lock is on from the next start, so the screens stay put instead of slowly turning.` plus, when the tool gave advice, `Coverage was thin: for a better result, run it again and <tool's movement advice>.` |
| Calibration failure / retry text (`cal_reason_text`) | `... (up_vs_yaw 6.10 deg, nod_level ...)`, `1.80 deg/s of movement`, `excursion 8.0 deg` | metrics parenthetical dropped; `1.80°/s of movement`, `Excursion 8.0°` |
| Calibration step detail while running | `head movement 0.80 deg/s (limit 1.5)`, `spread 1.2 deg, excursion 30.0 deg` | `head movement 0.80°/s (limit 1.5)`, `spread 1.2°, excursion 30.0°` |

Where the raw calibration figures went: when a calibration ends, the Diagnostics page's Activity
log gets a `head calibration detail: <tool detail>` (or `compass ...`) line above the existing
`... calibration finished: ...` line, so the metrics are one page away and never truncated by the
finish message. The failed panel still shows the tool's last console lines in mono as before.

Changed test expectation (deliberate): `app_selftest` "a retry reason survives the next waiting
line" now expects `1.80°/s of movement (limit 1.5)` (was `1.80 deg/s ...`). There were no
existing `format_layout_field_value` expectations; new cases pin `−45°`, `46°`, `0°` for −0.4,
`13°` for 12.6, `2.00 m`, `60 fps`. Existing `engine_status_summary` / `layout_summary` checks are
unchanged, and a new check pins the technical layout summary text.

One paint change, needed to avoid truncation: on the Layout page, when the hint and the summary do
not share a row and the summary is wider than the card (820x640), the summary now word-wraps onto a
second line instead of ending in an ellipsis. Wide layouts render exactly as before.

Left alone: the 2D arrangement canvas labels (`-45°`, `-90°`, paint code) keep the ASCII hyphen,
so they differ from the inspector's `−45°` by the minus glyph only. Tracking page live angles
(`+12.4°`) already used the degree sign.

Renders checked (`ui_snapshot`, 33 frames, PASS incl. contrast): home-running-1920x1080-150,
home-narrow-820x640, layout-1280x800, layout-narrow-820x640, tracking-calibration-saved-good,
wizard-3-head-saved-retry-light, wizard-3-head-failed-light, diagnostics-1280x800 (technical
Engine detail kept). Nothing truncates; Home hero, Layout card and calibration panels fit at
820 px.
