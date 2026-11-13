# Overnight improvement run — resume notes

Branch: `overnight-claude-improvements` (local only — never push, never merge into `main`).
Started 2026-11-12 ~22:55 local. Hard stop 10:30 local (2026-11-13) — changed from 07:30 by the user at 23:00.

## Status
- [x] Branch created, baseline build + 10/10 ctest green
- [x] Track A — startup-bias confirmation + 35 ms turn prediction: merged (e28e4ef), 11/11 green.
- [x] Track B — calibration --progress-json/--auto/--earbuds, mag status keys, opt-in earbud hard-iron tracker: merged (dd5e995), 11/11 green. Orientation-calibration averaging NOT done.
- [x] Track C — UI redesign + setup guide: merged (1ea0a5d), 11/11 green. Live-window check still needed.
- [ ] Integrate track branches into `overnight-claude-improvements`, full suite, docs, report

## Integration TODO (orchestrator)
1. DONE: Track B merged (imu_replay.cpp conflict resolved by keeping both tool sets).
   DONE 00:04: items 2-5 merged (7c9cc6e): predict-ms Off/20/35/50 (default 35), heading-lock badge,
   earbud switch (default off), in-app calibration in --auto mode with countdown + real cancel.
   (Originally delegated at 00:00 to an integration agent (worktree branch `worktree-agent-aa47a624fb8f72fa7`,
   reset onto overnight-claude-improvements). Merge it when done; if it stopped early, re-spawn from its branch.
2. Wire the UI's `EngineOptions.motion_prediction` (src/app_shell/ui_model.h, Tracking page) to the
   engine flag `--predict-ms N` (default 35, 0 = off) in the launch command (app_state.cpp).
   Decision: UI default ON at 35 ms (replay: turn error 2.92 -> 1.19 deg rms, reading bit-identical).
3. Wire `AppState::heading_lock` (Off/Locked/Disturbed/Relearning) to Track B's engine status keys.
4. Wire Track B's opt-in flags (e.g. `--mag-track-hard-iron`) into EngineOptions + Tracking page.
5. Check Track C's calibration wizard against Track B's real `--progress-json`/`--auto` behaviour
   (ENTER to advance, `q` cancels, `--mag --output <file>`).
6. Fold docs/overnight/track-*.md into AGENTS.md (bug table, envelopes, test map: new scenarios 20-23,
   `ui_snapshot` suite). Correct AGENTS.md section 7: the ~20 deg "thermal" drift on 2026-09-25 was
   mostly a contaminated startup bias (Track A: ~0.005 deg/s per degC measured).
7. Morning report: docs/overnight/REPORT.md.

## User decisions (asked before the run)
- Headphones: Huawei FreeBuds Pro 5 (in-ear wireless).
- No glasses available overnight: synthetic tests, replays and offscreen renders only. Never run the display takeover.
- UI: full professional redesign (keep every existing feature/setting).
- Business features: first-run onboarding only.
- Usage rule: stop at ~90% of the 5-hour window, write this file, resume after reset.

## Build (works on this machine)
`cmd /c %LOCALAPPDATA%\Temp\rayneo-overnight\build.cmd <source_dir> <build_dir> test`
(vcvars64 + vcpkg at `C:\Users\aksha\Desktop\GitHub\tools\vcpkg`, Ninja, RelWithDebInfo).
Build dirs live under `%LOCALAPPDATA%\Temp\rayneo-overnight\` (never inside the Desktop tree).

## How to resume
1. `git status`, `git log --oneline main..overnight-claude-improvements`, `git worktree list`, `git branch`.
2. Read the "Status" and "Log" sections here; each track agent writes `docs/overnight/<track>.md` in its branch.
3. Re-spawn any unfinished track from its branch (it keeps its commits), then integrate.

## Log
- 22:55 baseline green (41 s build, 10/10 tests).
- 22:58 Tracks A/B/C launched as Opus (high) agents in separate git worktrees (branches `worktree-agent-*`).
  Each self-stops at >=85% of the 5-hour window and writes `docs/overnight/track-<x>.md`.
  The 5-hour window resets 03:40 local (GMT); scheduled task `rayneo-overnight-resume` nudges the
  orchestrator session at 03:45. A 07:15 wrap-up nudge was refused by the permission classifier,
  so the orchestrator times the 07:30 stop itself.
- Plan: Pro plan, weekly limit was 46% at start; keep weekly below ~90%.
- 23:00 User moved the hard stop to 10:30 local. Second resume nudge `rayneo-overnight-resume-2` scheduled
  for 08:50 (after the next 5-hour reset); it runs in default permission mode and may pause for approval.
- 23:35 Track C finished (4 commits) and merged into the overnight branch; 11/11 green. Renders in
  `.claude/worktrees/agent-af4658d0d73d6c1e7/scratch/ui/`. 5-hour usage 54%, weekly 53%.
- 23:50 Track A finished (4 commits) and merged; 11/11 green. 5-hour usage 70%, weekly 55%.
- 00:00 Track B finished (4 commits) and merged; 11/11 green. Integration agent launched (UI <-> engine wiring).
  5-hour usage 72%, weekly 55%.

- 00:04 Integration merged (7c9cc6e); 11/11 green. 5-hour usage 81% -> PAUSED until the 03:40 reset
  (nudge at 03:45). Weekly 56%.
- 03:46 Resumed (5-hour 3%, weekly 57%). Launched R2-A (review+fix), R2-B (orientation calibration
  averaging/quality), R2-C (startup long-rest guard) as Opus high agents in new worktrees; each stops at 80%.
  Orchestrator: AGENTS.md folded (4241552); docs/overnight/REPORT.md drafted (round-2 section pending).
- 04:02 R2-B merged (61150a9): opt-in `--repeats N` (default 1) with robust averaging + quality band in UI; 11/11 green.
  Open decision for the user: use `--repeats 3` in the in-app wizard (+21 s, modelled error -40%). 5-hour 20%, weekly 60%.

## Round 2 plan (start at 03:45; window 2 ~03:40-08:40, window 3 ~08:45-10:30 hard stop)
Window 2, in parallel, each in its own worktree reset onto overnight-claude-improvements:
- R2-A Review+fix agent (Opus high): adversarial review of `git diff main..overnight-claude-improvements`
  (live UI message paths: hover/drag/wheel/tray/DPI/timer stop, calibration process handling; estimator
  startup confirmation + prediction; mag tracker). Fix only confirmed bugs, each with a regression test.
- R2-B Calibration agent (Opus high): orientation calibration accuracy (repeat averaging, outlier rejection,
  consistency/quality score surfaced through --progress-json quality and the UI).
- R2-C Estimator agent (Opus high): startup single-long-rest-episode guard (Track A "known gap"), proven on
  the startup audit + synthetic scenario, without regressing any envelope.
Then orchestrator: fold docs/overnight/*.md into AGENTS.md (bugs 26+, envelopes, test map incl. scenarios
20-23 and ui_snapshot; correct the "thermal drift" sentence), write docs/overnight/REPORT.md.
Window 3: merge leftovers, final full suite, final report. Stop at 10:30.

## Round 2 ideas (backlog)
- Code review of everything merged (bugs in the redesigned UI's live paths: hover/drag/tray/DPI/timer; estimator changes).
- Track B item 4: orientation calibration averaging + quality/consistency scoring.
- Track A next: guard for the single-long-rest-episode startup case; seed bias from last session.
- Fold docs/overnight/*.md into AGENTS.md; write docs/overnight/REPORT.md (morning report).
