# R6-D review: painting, widgets, theme, animation, setup guide, ui_snapshot

Reviewer R6-D, branch `overnight-claude-improvements` (worktree branch
`worktree-agent-a1309a3362a84bd0a`). Area: `ui_paint.*`, `ui_paint_internal.h`,
`ui_pages.cpp`, `ui_page_layout.cpp`, `ui_widgets.*`, `ui_onboarding.cpp`,
`ui_theme.*`, `ui_anim.*`, `ui_help.h`, `src/tools/ui_snapshot.cpp`, `CMakeLists.txt`.
No window was opened and no display was touched: every claim below is a code trace or an
offscreen `ui_snapshot` render (PNGs in `%LOCALAPPDATA%\Temp\rayneo-overnight\r6d-ui`).
ctest 11/11 at every commit, MSVC Ninja RelWithDebInfo, `/W4 /permissive-`, no new warnings.

## Fixed (each with a check that fails without the fix)

| Commit | Finding | Evidence |
|---|---|---|
| `06990a8` | **Tab focus trap on Home when the layout file has a problem.** The Readiness row for a broken layout file painted "Open layout" with `kUiGoBase+Layout`, the same id as the Layout card's "Edit layout" button. `cycle_focus()` (`app_window.cpp`) uses `std::find` for the first copy, so forward Tab from the second copy goes back to the element after the first and loops; it never reaches the sidebar again. Hover also lit both buttons. The row no longer has its own button. | New shot `home-layout-problem-1280x800`: before, "hotspot id 1411 is registered twice"; after, PASS. |
| `09f0ce0` | **Layout page lost its editor and inspector at the minimum window size.** At 760x560 (client ~744x521) with six user presets the presets card wrapped to four rows, the main row fell under 160 px and `paint_layout_page()` returned early: no arrangement editor, screen chips, Add/Remove or field sliders (also at 200 %). The presets card is now capped so the main row keeps 280 px; its chips scroll inside it, using the page scroll portal that the Layout page otherwise leaves hidden. Windows where everything fits paint as before. | New check: every Layout-page shot must register `kUiEditorCanvas`; new shots `min-<page>-744x521` and `min-layout-1488x1042-200-light`. Before: "the Layout page painted no arrangement editor" at 100 % and 200 %; after, PASS. |
| `dc2262b` | **Sidebar engine card said a grey "Stopped" after a failed run.** It printed `view.busy ? view.label : "Stopped"`. Once a failed process is reaped, `busy()` is false while the snapshot stays `Failed`, so every page except Home looked calm after a crash (the old header chip said Failed). `status_card()` now keeps "Failed" with the error colour. | Per-shot pixel check of the card's dot: `sidebar-failed-home/-layout` got 134,141,156 (faint grey) before and the error colour after; `sidebar-stopped` stays grey. |
| `8e2619d` | **Slider knob not where a press maps.** The redesigned `draw_slider()` inset the track by the knob radius (8 px at 100 %), but `AppWindow` maps a press at x to `(x - rect.left) / width` of the full hit rect and applies it on button-down. Pressing on the knob jumped the value (10 % brightness read back as about 12 %, 100 % as 98 %; several degrees on yaw) and the knob trailed the pointer near both ends. Main's slider drew the track across the full rect. The track spans the hit rect again. | New shot `comfort-slider-ends-1280x800` finds each knob's white centre and maps it back: before, 8.0 px -> 0.022 for 0.000 and 364.0 px -> 0.978 for 1.000; after, within 1.5 px. |
| `00aa639` | **Long screen ids lost their angle in the editor.** Both views formatted "id  yaw" into a 64-wide `swprintf` buffer; ids may be 64 characters, so for ids over about 57 characters the angle disappeared and long labels ran across the canvas. `editor_screen_label()` shortens the id to 24 characters and always keeps the angle. | `ui_snapshot` label check: old formatting gave a 62-character label for a 56-character id and no angle for 64; now 12 / 30 / 30 characters, angle present. |

## Not fixed (other owners, or not worth the risk)

| Where | Status | Finding |
|---|---|---|
| `app_window.cpp:1743` | CONFIRMED (Haiku sweep, spot-checked) | `kUiEditorDimToggle` is still handled but nothing paints or registers it since the 2D/3D segmented control replaced it. It is a dead handler; `kUiMotionPrediction` (`app_state.h:82`) is also unused. app_window owner. |
| `app_state.cpp:79` `start_block_reason` | PLAUSIBLE | Home says "Ready to start" while Readiness shows "Layout file: Problem" (see `home-layout-problem` PNG). A broken layout file and a missing head-tracking sensor are not start blocks. This may be the same on main. app_state owner. |
| `ui_pages.cpp` `settings_card_shortcuts` | CONFIRMED, minor | The old footer listed `Ctrl+Alt+Q` (engine quit, still registered in `spatial_desk.cpp:605`), Space (activate) and the mouse wheel. The new Shortcuts card has none of them. It is text only; no fix without a meaningful regression test. |
| `ui_pages.cpp` `paint_diagnostics` | by design | Activity list is capped to the newest 14 events. Main showed the whole list; track-c does not mention the cap. |
| `ui_paint.cpp` header actions | by design (track-c) | Refresh appears only on the Home and Diagnostics headers (main had it on every page). The header "Last: workspace/preview" mode chip is gone; `last_mode` is still saved but no longer shown. |
| `ui_widgets.cpp` `format_wide` | PLAUSIBLE, low | `_vsnwprintf_s(..., _TRUNCATE)` returns -1 on truncation and the function then returns an empty string. That only matters past 511 characters; no current caller gets close. |
| `ui_page_layout.cpp` header Save/Revert | REJECTED | Always enabled, unlike the fullscreen pair that needs `layout_dirty`. Saving a clean layout only re-applies it. Harmless. |
| `ui_anim.cpp` | REJECTED | `approach()` snaps within epsilon, so `animating()` settles. dt is clamped to 0.1 s and dt = 0 keeps the value. The one fraction target (`CalRunState::fraction`) cannot be NaN. Bug 31's visibility gate holds. |
| GDI/GDI+ lifetimes | REJECTED | `FontSet::create` destroys first (DPI change). Every `SelectObject` is restored. Back buffer, fade buffer and the snapshot DIBs are released on every path. `GdiplusSession` outlives the window in `main.cpp` and `ui_snapshot`. `push_clip`/`pop_clip` use a SaveDC stack. |
| `paint_help_bubble` id range | REJECTED | `kUiHelpBase + 7*64 = 1148` stays below `kUiSplitBase = 1200`. |
| Measure vs draw passes (`paint_calibration_run`, cards) | REJECTED | The measure pass and the draw pass advance `y` the same way in every branch. Switch-row heights use the same text width as `switch_row()`. |

## Cleanup notes (not changed)

- Each `Canvas` primitive builds a new `Gdiplus::Graphics` (hundreds per frame while animating at ~66 Hz).
  One `Graphics` per paint pass (or per card) would be cheaper; the cost is not visible today.
- `health_items()` runs `std::filesystem::file_size(engine_log)` on every Home and Diagnostics paint,
  including animation frames, and the Diagnostics page re-splits the whole engine-log tail every paint.
  Both could be cached on the 1 s poll.
- The 2D editor's tick labels use an ASCII hyphen (`-90°`) while value formatters use U+2212.

## Removed-behaviour mapping (track-c table vs code)

A Haiku sweep compared every painted control on main with HEAD, and I spot-checked the load-bearing rows.
Every row in track-c's table is where it claims. The items it does not mention are listed above:
Refresh on only two pages, the missing mode chip, the 14-event cap, the dropped Space/wheel/Ctrl+Alt+Q
hints and the sidebar Failed state (now fixed). Every id that HEAD registers has a handler in `execute()`
or a pointer path. The only id handled but never painted is `kUiEditorDimToggle`.
