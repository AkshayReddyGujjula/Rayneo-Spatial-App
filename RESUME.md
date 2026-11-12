# Overnight improvement run — resume notes

Branch: `overnight-claude-improvements` (local only — never push, never merge into `main`).
Started 2026-11-12 ~22:55 local. Hard stop 10:30 local (2026-11-13) — changed from 07:30 by the user at 23:00.

## Status
- [x] Branch created, baseline build + 10/10 ctest green
- [x] Track A — startup-bias confirmation + 35 ms turn prediction: merged (e28e4ef), 11/11 green.
- [ ] Track B — calibration + earbud-magnet robustness (Opus agent, own worktree branch)
- [x] Track C — UI redesign + setup guide: merged (1ea0a5d), 11/11 green. Live-window check still needed.
- [ ] Integrate track branches into `overnight-claude-improvements`, full suite, docs, report

## Integration TODO (orchestrator)
1. Merge Track B branch `worktree-agent-adb4e6e0fd7294fe2` (if it stopped early, re-spawn it from its branch).
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
