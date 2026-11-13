# R6-A: review of the estimator, smoother, IMU source, magnetometer and engine

Scope: `git diff main...overnight-claude-improvements` for `src/imu/pose_estimator.*`,
`pose_smoother.*`, `imu_source.*`, `mag_heading.*`, `src/app/spatial_desk.cpp`,
`src/app/engine_protocol.h`, `src/tools/imu_replay.cpp` and their tests. Every hunk was read
with its enclosing function; removed lines were traced to where their invariant is
re-established; callers were checked with grep. Build: MSVC Ninja RelWithDebInfo, own build
directory, 11/11 ctest green before and after, no warnings.

## Confirmed and fixed

### 1. An IMU reconnect's view step was predicted as a turn (commit `64d1342`)

- **Symptom.** When the glasses' IMU stream drops (unplug, or the 2 s stream timeout in
  `ImuSource::run`), the engine keeps rendering the last published pose while ImuSource
  reconnects, warms up (4 s) and recalibrates. The new estimator publishes from a fresh
  reference (`set_reference` on its first sample), so its first pose is straight ahead and the
  smoother's input steps by however far the wearer was looking, in one frame. The turn
  prediction differentiates consecutive inputs and read that as a 2700 deg/s turn: the view
  was thrown far past straight ahead for several frames.
- **Root cause.** Bug 30's pattern on a path it did not cover: only recenter (`reset()`) and the
  four tracking toggles (`restart_prediction()`) told the smoother that a step was not motion.
- **Fix.** `ImuSource` publishes a per-connection session epoch with each pose, inside the pose
  seqlock (`orientation(uint32_t& epoch)`), so pose and epoch are always read as a pair. The
  engine passes it to `PoseSmoother::note_source_epoch()` before each update; a changed epoch
  restarts the prediction exactly like `restart_prediction()`. The 1-euro and the reading hold
  still ease the step in as before.
- **Test.** `pose_scenarios` 29 (45 deg reconnect step, medium hold, 60 Hz, 10 s of frozen pose
  first): overshoot past straight ahead 0.000 deg without prediction, **23.904 deg** predicted with
  no epoch handling (the check fails with a no-op `note_source_epoch`), 0.000 deg with it; the
  settled pose matches the no-prediction run. Scenarios 1-28 and `pose_selftest` print output
  identical to the branch tip (diffed).
- **Needs live verification:** unplug/replug the glasses while looking at a side screen.

### 2. A recenter landing after the smoother reset was predicted as a turn (follow-up commit)

- **Race.** The recenter handlers (`spatial_desk.cpp`, hotkey and controller) call
  `view_smoother.reset()` at once. The IMU thread applies `recenter()` only at the top of its next
  loop, and it publishes the recentered pose one sample later: about 2-4 ms normally, and up to
  the 100 ms read timeout on a USB stall. A frame whose late pose is sampled inside that interval
  restarts the smoother from the pre-recenter pose. The view then steps one frame later, and the
  prediction read that step as a turn. Whether the race happens on hardware depends on the
  capture pass plus `wait_for_frame()` being shorter than that latency. That was not measured.
  The consequence, once the race happens, is reproduced below.
- **Fix.** `ImuSource::run` increments the session epoch right after `estimator.recenter()`, so
  the epoch changes on exactly the pose where the view steps, and `note_source_epoch()` restarts
  the prediction there. When there is no race, the reset has already restarted the smoother, so
  the changed epoch has no effect. The 1-euro glide of a late step predates this branch and is
  unchanged.
- **Test.** `pose_scenarios` 30 (30 deg recenter, medium hold, 60 Hz): the smoother is reset, one
  frame samples the pre-recenter pose, then the recentered pose arrives. Overshoot past straight
  ahead is 0.000 deg without prediction, **12.936 deg** predicted when the epoch does not change
  on the recenter (the check fails when the scenario's epoch is not bumped), and 0.000 deg with
  the epoch change. The settled pose matches the no-prediction run. As with scenario 29, the test
  drives the smoother with the epoch sequence that `ImuSource` publishes. `ImuSource` itself
  needs the HID device, so no test runs it. `pose_selftest` and scenarios 1-29 print identical
  output.

## Plausible, not fixed

- **Hard-iron tracker bound** (`mag_heading.cpp` `decide()`, opt-in, off by default): the
  `max_offset_ut` bound is checked when a step is detected, but adoption then refines the
  offset to `c_before + a h` with `a` up to `max_scale` 1.5, so the departure can exceed the
  bound by up to 0.5 |h| (at most 4 uT with `max_step_ut` 8). Still bounded; no failure shown.

## Reviewed, rejected

- **Startup confirmation (bugs 26/28/32 code).** Re-derived independently of r3: the frames
  (raw sensor frame throughout), the rotation sandwich `conj(mid) g0 mid` with
  `mid = first_half * q * conj(window_half)`, the quaternion square root, the clamp on every
  bias write (invariant 5), `reset_escape()` on every reseed/revert (invariant 6), the window and
  episode reset on a >= 50 ms gap, and `configure()` resetting every new field. One detail:
  `single_rotation_` double-counts the first window's last sample and omits the current
  window's last sample (a one-sample, ~2 ms shift of the integration interval); at the 3 deg/s
  rest ceiling that is <= 0.006 deg, negligible against the 5 deg / 0.3 gates.
- **Status heartbeat lifetime.** Started after the first publish, stopped before the render
  loop, and the destructor joins it on every early return (it outlives the `TopologyGuard`
  restore on failure paths, repeating the published `failed` state, which is correct). Lock
  order is mutex_ -> g_status_write_mutex in the thread only; `stop()` and `publish_status()`
  never nest, so no deadlock. The file is not refreshed during the shutdown restore; the
  controller shows its own Stopping state there, so nothing visible depends on it.
- **Prediction maths.** Body-frame velocity from `conj(prev) * input` with hemisphere flip,
  lead applied as `held * lead`, `dt <= 0` / `> 0.1 s` reseeds, NaN cannot reach the output.
  `lead.w >= 1.0f` returns the unpredicted pose when `cos(half)` rounds to 1, dropping a lead
  below ~0.028 deg once while speed passes ~14 deg/s; invisible, and the gain check above it
  already returns the exact identity for every closed-gate case (cleanup only).
- **Engine arguments and status.** `--predict-ms` clamps to 0..100 and cannot produce NaN; the
  `mag` state names cover `relearning`; phase publishing compares strings and writes on change
  and every 0.25 s during warm-up / hold still.
- **Hard-iron tracker maths.** Re-derived `close_window`: `hg`, `sep = |h|^2 - |Mbar h|^2`,
  `var_revert = var0 + 2 hg + sep` and `a* = 1 + sum(hg) / sum(sep)` are the exact one-parameter
  least squares; the step detector rotates the estimates by `step^T` (earth-fixed vector in a
  body frame composed on the right), consistent with the fit.
- **`imu_replay` additions** (startup audit, photon-error evaluation, tracker flags): diagnostic
  only; nothing found that changes a documented number.

## Cleanups (not applied)

- `MagHardIronTracker::add_sample`: `h` and `(void)h;` are dead.
- `confirm_single_episode` returns a bool nobody reads.
- `pose_smoother.cpp` `lead.w >= 1.0f`: redundant with the gain early return (see above).
