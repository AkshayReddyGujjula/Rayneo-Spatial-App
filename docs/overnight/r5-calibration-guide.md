# Round 5-A: live calibration guide (2026-11-13)

The owner asked for calibration guides: "a red line in the middle with the ball moving, so I can
actually see and move my head accurately". This round adds a live guide on the glasses and in the
PC calibration panel, driven by a new display-only `@@live` stream from the tool. No glasses were
available: everything below is verified synthetically (selftests and offscreen renders).

Commits (branch `worktree-agent-a9869d24a7b66e3dd`, based on `overnight-claude-improvements`):

1. `Calibration tool: @@live guide lines (display-only head position per step)`
2. `Controller: live calibration guide on the glasses and in the calibration panel`
3. `Calibration guide: the PC panel no longer repeats the step label`
4. this document and `docs/orientation-calibration.md` section 10

## 1. Research (short)

Sources, all found with a web search on 2026-11-13:

- Functional sensor-to-segment calibration defines the vertical from an upright static posture
  and a second axis from a movement about one anatomical axis; the movement must stay mostly about
  that one axis to identify it (Frontiers in Bioengineering 2024,
  https://www.frontiersin.org/journals/bioengineering-and-biotechnology/articles/10.3389/fbioe.2024.1385750/text;
  MDPI Sensors 23(18):7987, https://mdpi.com/1424-8220/23/18/7987).
- Single-axis functional calibration suits hinge-like motions; for multi-axis joints the motion
  has to be kept about the wanted axis (MDPI Sensors 16(12):2011, https://mdpi.com/1424-8220/16/12/2011).
- Functional calibration beats a posture-only (N-pose) calibration in precision when compared with
  optical motion capture (Bonfiglio et al. 2024,
  https://iris.unitn.it/retrieve/a64e0285-c39e-4c62-aba3-6405dbb13c67/).
- Repeatability was studied (PMC7039222, https://pmc.ncbi.nlm.nih.gov/articles/PMC7039222), but
  the search found no study quantifying amplitude or repeat count for single-axis movements, so
  those choices below are reasoned from first principles and from this repository's own models.

First-principles reasoning (stated as such):

- **Axis purity is the error.** The measured axis is the principal direction of the gyro over the
  gesture. Off-axis rotation of fraction f tilts it by about atan(f): 10% is about 6 deg, 15% about
  8.5 deg of axis error per gesture, before averaging. The tool's dominance gate (4.0, an energy
  ratio) still accepts 50% amplitude mixing, so passing the gate is not the same as accurate.
  Real-time feedback on off-axis motion is the most direct aid: hence the ball's perpendicular
  displacement, the colour change and the OFF_AXIS hint at 12%.
- **Amplitude.** Gyro noise and the brief start/stop transients are roughly fixed per gesture, so
  the signal-to-noise ratio grows with excursion; the existing quality score already gives full
  travel marks at 25 deg. Very large excursions invite compensating torso or shoulder motion, so
  the targets are 25 deg (turn) and 20 deg (nod, tilt), with the real 12 deg gate shown as the
  minimum mark.
- **Speed.** Smooth, moderate rotations keep the motion inside the 2.5 deg/s detector and away
  from whip-like accelerations that mix axes; SLOWER fires above 80 deg/s.
- **Return to neutral and stillness.** The repeat splitter needs each gesture to come back within
  30% of its peak; the still step needs gyro rms under 1.5 deg/s over a quiet 1.5 s window. The
  guide counts repeats with the same return rule and shows the stillness as a ring that tightens.
- **Repeats.** Already decided (r2-calibration.md: 1.90 -> 1.14 deg modelled error at N = 3). The
  guide shows "n of 3" dots so the wearer does all three instead of one.
- **Headset calibration UX.** The display is head-locked, so a world-locked target cannot exist;
  the ball is a head-pointing cursor relative to the step's start pose (turn left: ball left; nod
  down: ball down; tilt right: ball clockwise on an arc). On birdbath optics black is transparent,
  so the guide is bright strokes on black and never covers the user's view with a fill.

## 2. `@@live` contract

See `docs/orientation-calibration.md` section 10 (pinned by `orientation_calibration_selftest`).
About 22 lines per second (45 ms throttle), roughly 300 bytes each: about 7 KB/s for the 90 s run.
The tracker (`src/imu/calibration_live.*`) integrates (gyro - still-step bias) as a quaternion
from the step's start pose and expresses its rotation vector in the guide frame. During `--auto`
countdowns the tool now reads the IMU (to draw the head live); those samples were previously left
in the HID buffer and are never recorded either way. Non-`--progress-json` runs print nothing new.

## 3. What the wearer sees

**On the glasses** (`src/app_shell/cal_guide_window.*`): a borderless, topmost, non-activating,
click-through (layered + transparent, `HTTRANSPARENT`) tool window covering the monitor found by
the Readiness "Glasses display" rule (`find_glasses_monitor_rect`, virtual displays excluded). It
opens with the first `@@live` line of a head calibration and closes on done/failed/
cancel/app exit (also a backstop on the health timer). No display settings change and no other
window moves. No glasses monitor: nothing opens (probed at most every 2 s).

- Top band: "Step n of 4", the big instruction (step name while waiting, the live hint while
  recording: "Turn further", "Back to centre", "Keep your head level", "Slower", "Other way: turn
  left", "Hold still"...), the how-to sentence during the countdown, the retry correction in amber,
  and a large countdown number.
- Guide: red line through the centre (horizontal for the turn, vertical for the nod, a red arc for
  the tilt with a line from its centre to the ball), a grey mark at the 12 deg gate and a green
  mark with its angle at the target, a hollow marker at the furthest point reached, and the ball:
  green on-axis, amber off-axis / wrong way / too fast, grey during the countdown. The still step
  shows a crosshair, a small green goal ring and a steadiness ring that shrinks to it as the gyro
  rms falls (full size at the 1.5 deg/s gate).
- Bottom: one dot per repeat, filled as each out-and-back is counted.
- Scale: pixels per degree from the layout FOV (46 deg default), reduced so the target plus 5 deg
  of overshoot fits in 90% of the half extent (turn on 1920 px: 28.8 px/deg; nod: ~19 px/deg).
  The tilt arc draws twice the roll angle. The ball is eased at ~40-50 fps (tau 50 ms) between the
  22 Hz lines; the window is double-buffered and its timer exists only while it is open.

**On the PC** (Tracking page calibration card and the setup guide's head step; head calibration only, the compass flow keeps its text view): the same painter
in a ~200 px tall theme-coloured box (canvas background, error-red line, ok/warning ball) under the
prompt, while a step waits or records. Older tools without `@@live` keep the previous text-only view.

**Retry correction** (`cal_retry_tip`): after a failed attempt the guide names one correction from
what it saw for at least ~0.5 s of that attempt, in priority order: wrong way (names the direction),
off-axis (step-specific: chin level / nod straight down / nose ahead), fewer repeats than asked
("go a little further each time, past the first mark"), too fast. It stays for the retried attempt.

**Pre-check / demo text**: during each countdown the guide shows how to do the step ("Sit upright,
look at a fixed point straight ahead", "Turn left as if saying no, chin level, until the ball passes
the green mark (about 25 degrees), then come back to centre. Do it 3 times.").

Gates, the default flow length and the calibration maths are unchanged.

## 4. Tests

- `orientation_calibration_selftest` (fails without the change: the symbols do not exist; the
  behaviours are asserted): pinned `@@live` line and JSON safety; right-handed guide frame,
  levelled and fallback forward; bias removal; pure turn along/off-axis, speed, FURTHER ->
  BACK_TO_CENTRE -> one repeat -> DONE; a turn with 15% roll gives OFF_AXIS (off 3.70 deg) while 8%
  does not; nod down along +20 / pitch -20; tilt right along +15, out-and-back counted, an 8 deg
  tilt returned early not counted (FURTHER); WRONG_WAY; SLOWER at 150 deg/s; still step STEADY vs
  HOLD with the gyro rms (2.12 deg/s for a 3 deg/s sinusoid); waiting gives GET_READY and counts
  nothing.
- `app_selftest`: the tool's pinned line parses field for field; console and progress lines are not
  live; malformed kind/state/JSON; clamping and defaults; live lines never reach the console tail;
  an older tool keeps the text-only view; countdown, step name and how-to while waiting; stale
  samples (> 1 s) not drawn; hint headline and "1 of 3" while recording; off-axis warning tone; a
  retry after off-axis lines yields the chin-level tip, kept for the retried attempt and cleared on
  ok; tip priorities; degrees -> pixels (28.8 / 41.7 / 19.4 px/deg, FOV fallback); ball direction,
  clamping and the tilt arc; steadiness ring; colour state; hint text per step.
- `ui_snapshot`: seven glasses renders at 1920x1080 (still, turn on-axis, turn off-axis, nod, tilt,
  waiting countdown, retry tip) with pixel checks (background black, at most a quarter of the frame
  lit, red line present, a lit ball at the computed position), and panel renders on the Tracking
  page (each state, plus light theme) and in the setup guide (normal and narrow), with the existing
  hotspot and WCAG checks. All PNGs were looked at.

ctest: 11/11 green, `/W4 /permissive-` without new warnings.

## 5. Needs live verification (glasses on a head)

- The overlay actually appears on the glasses monitor, stays click-through and never steals focus
  from the controller (the Cancel / Start now buttons must keep working); it closes on every exit
  path, including a tool crash.
- Directions: turning left moves the ball left, nodding down moves it down, tilting right moves it
  clockwise, for both a valid existing calibration file ("file" frame) and none ("nominal").
- Comfort of the scale and of the targets (25 / 20 / 20 deg) and the 80 deg/s SLOWER threshold.
- Readability of the type and colours on the birdbath optics (black must look transparent).
- That reading the IMU during the countdown does not disturb the recording (sample counts and
  rates in the console should match earlier runs).
- Whether the guide measurably improves first-time success and the stored quality; compare a few
  runs with and without the window.
- In the setup guide on small windows the guide sits below the fold of the step body (scrollable);
  the glasses window is the primary view.
