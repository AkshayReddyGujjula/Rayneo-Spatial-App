# Overnight improvement run — morning report

Run: 2026-11-12 22:55 to 2026-11-13 10:30 (local). Branch `overnight-claude-improvements`, local
only: nothing was pushed, nothing was merged into `main`. To throw it all away, just stay on `main`;
to try it, `git switch overnight-claude-improvements` and build as usual.

**No glasses were connected**, so nothing here has been seen on a head. Everything was checked with
the synthetic test suites, replays of your recorded session (`dist/reference-logs-2026-09-25.zip`)
and offscreen renders of the UI. The checklist at the end is what to try first with the glasses on.

## What you get

### 1. A redesigned controller app
- Sidebar with six pages: **Home, Layout, Comfort, Tracking, Diagnostics, Settings**
  (Ctrl+1..6). Every previous button and setting is still there (the mapping is in
  `docs/overnight/track-c.md`).
- Bigger type, more spacing, rounded anti-aliased controls, dark, light or "match Windows" themes,
  smooth hover/slide/page animations (they stop using CPU when nothing moves, and respect the
  Windows "show animations" setting), sharp at any display scaling, compact icon sidebar on narrow
  windows.
- **First-run setup guide**: welcome, glasses checks with plain-language fixes, head calibration,
  optional compass calibration, layout preset, done. It appears once (your existing setup will see
  it on the next launch; *Skip setup* closes it for good) and can be re-run from Settings.
- **Calibration inside the app**: progress, step n of N, what to do, a 5-second countdown so you can
  hold the pose without touching the keyboard (or *Start now*), quality scores, plain-language
  failure messages, and a Cancel that stops the glasses' sensor stream properly.
- Screenshots: run `ui_snapshot --out scratch\ui` from the build folder (PNGs in `scratch/ui`).

### 2. Less drift at startup
Your recorded session's biggest drift (+35 deg in the first 5 minutes) was **not** heat: the
1.3-second "hold still" window at startup caught a slow head roll and learned it as gyro bias.
The app now double-checks the startup bias against your next rests during the first minute and
corrects it in one step if it was wrong. On 870 simulated starts from your recording: average
first-minute drift 9.2 -> 7.1 deg, first three minutes 15.7 -> 11.0 deg, startup time unchanged.

### 3. Screens keep up with fast head turns
**Turn prediction** (Tracking page: Off / 20 / 35 / 50 ms, default 35) shows the world slightly
ahead during quick turns. On your recording: turning error 2.9 -> 1.2 deg at medium stabilisation.
It does nothing while you read (reading output is identical to before).

### 4. Earbuds and the compass
- Your FreeBuds' magnets move with your head, so to the compass they look like a shifted offset.
  **The fix that works today: calibrate the compass with the earbuds in** (tick *Earbuds in*), the
  way you normally wear them. Their magnets then become part of the calibrated offset.
- For putting earbuds in or taking them out mid-session there is an **experimental** switch,
  *Earbud magnet compensation* (off by default). In replays with simulated earbud steps it cut the
  heading error from 4.9 to 1.9 deg on average, but on your real recording it also reacted to one
  unexplained magnetic event, so it stays off until it has been tried on the glasses.
- The Tracking page now shows the compass state live: gyro only, finding north, locked,
  interference, checking a magnetic change.

### 5. Round 2: review, more accurate calibration, more startup drift fixed
- **Bugs found by a code review of everything above and fixed, each with a test:**
  - turn prediction overshot ~13 deg when you switched yaw/pitch tracking back on;
  - the app kept animating ~66 times a second while hidden in the tray;
  - the setup guide could overwrite an `app.json` that had failed to load;
  - the workspace could be started while a calibration was running (both would fight over the
    glasses' sensor);
  - a slider/screen drag could stick after Alt+Tab; a button could stay drawn pressed;
  - two test suites failed when run at the same time (one cause was real: session logs could be
    left unarchived).
- **Visual check of every screen** (both themes, 150 % scaling, narrow windows): all text now
  meets the WCAG AA contrast standard (the dark-theme primary button is a slightly deeper blue),
  cut-off help text now wraps, screen labels in the arrangement editor no longer sit on the
  screens, and the setup-guide step labels line up. The renderer now fails the build if any colour
  pair drops below AA. Before/after images: `docs/overnight/ui-qa/`.
- **Plain wording on the normal pages:** "running | workspace | 3 screens | 60 fps | VDD desktops"
  now reads "Workspace · 3 screens · 60 fps · virtual desktops"; layout summaries read
  "3 screens · arc −45° to 45° · 46° field of view · 120/30/1 fps capture"; after a calibration
  you get a plain sentence instead of `up_vs_yaw 6.10 deg, nod_level …`; angles use ° everywhere.
  The technical versions are still on the Diagnostics page and in the logs.
- **More accurate head calibration (opt-in):** `--repeats N` has you do each turn, nod and tilt N
  times; outliers are thrown out and the rest averaged. Simulated error 1.9 -> 1.1 deg with 3
  repeats (about 21 s longer). After every calibration the app now shows **Calibration quality:
  Good / Fair / Retry recommended**.
- **More startup drift fixed:** if the "hold still" moment caught a slow head roll and you then
  sit still for 10 s or more, the app now uses gravity to prove the startup reading was wrong and
  fixes it straight away. On your recording's own start: 60-second drift 20.3 -> 14.7 deg, three
  minutes 32.8 -> 16.0 deg. Trade-off, measured and bounded: if a bad start *and* a very slow
  level turn happen in the same still period, it can learn the turn (+3 deg in the worst
  simulated case).
- **Independent check of that new maths:** a second reviewer found one real flaw (a timing
  mismatch that could let a slow nod "correct" a startup reading that was already right, costing
  up to ~2.8 deg of drift) and fixed it with a new test; the calibration averaging was checked and
  found sound.

### 6. A "Hold your head still" prompt at start-up
The +35 deg drift in your recording started because your head moved during the 1.3-second moment
when the app measures the sensor at start-up, and nothing told you to keep still. Now:
- Before you press Start, Home says to look straight ahead and keep still when asked.
- While the workspace starts, Home shows the steps: Screens -> Glasses sensor -> Warm-up -> **Hold
  your head still** (large, with a filling bar that restarts if you move), then it is running.
  The screen setup step no longer looks frozen during the 13-35 s display takeover.
- The glasses themselves show no prompt yet (the renderer has no way to draw text). A possible
  follow-up: keep the screens dimmed until tracking starts, as a "you can move now" cue.

### 7. A live guide while you calibrate (your morning request)
While head calibration runs, the glasses show a guide on a black (see-through) background:
- a **red line** for the movement the step wants (across for the turn, up-down for the nod, an arc
  for the tilt) and a **ball that follows your head** live;
- a grey mark at the minimum (12°) and a green target mark (25° turn, 20° nod and tilt), plus a
  marker at the furthest point you reached;
- the ball turns **amber** when you drift off the line (e.g. tilting while you turn), go the wrong
  way or move too fast, with a big hint: "Keep your head level", "Turn further", "Slower",
  "Back to centre";
- a ring that tightens as you hold still for the still step, dots for each of the 3 repeats, and a
  countdown that explains how to do the next step;
- after a failed attempt, one specific correction based on what went wrong.
The same guide appears smaller in the calibration panel on the PC. Research behind it (cited in
`docs/overnight/r5-calibration-guide.md`): accuracy depends most on keeping each movement about one
axis (15 % off-axis motion tilts the measured axis by about 8.5°), on a reasonable range (about
20-25°), smooth moderate speed and a full return to centre - exactly what the guide shows. The
calibration maths and pass/fail checks are unchanged.

## Your decisions
- **Calibration repeats in the app: 3** (your call in the morning). In-app head calibration now
  asks for each turn, nod and tilt three times (about a minute and a half in total); the setup
  guide and Tracking page say so. The console tool's own default stays 1.

## Try this first with the glasses
1. Open the app: does the setup guide appear, do hover/drag/scroll feel right, does CPU go to ~0 %
   when idle?
2. Start the workspace and watch Home: do the start-up steps advance, does "Hold your head still"
   appear while you are looking at the PC screen, does the bar restart if you move?
   (Engine log: `startup phase:` lines.)
3. Run head calibration from the app: does the guide appear in the glasses, does the ball move the
   right way for turn, nod and tilt, does the countdown give you enough time, does Cancel stop
   cleanly?
4. Start the workspace a few times: is the view steadier in the first minute? (Engine log line:
   `startup confirmation: reseeds=N reverts=M`.)
5. Turn your head quickly and stop: if screens bounce past, lower Turn prediction to 20; if they
   still trail, try 50.
6. Calibrate the compass with your earbuds in, then check the compass badge says Locked.

## Where the details are
`docs/overnight/track-a.md` (tracking), `track-b.md` (calibration, compass, earbuds),
`track-c.md` (UI and integration), `r2-*.md` (round 2), `AGENTS.md` sections 3, 5, 6 and 10.
