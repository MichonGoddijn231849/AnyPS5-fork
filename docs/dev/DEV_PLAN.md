# AnyPS5 developer plan

Draft of 2026-10-09. It is the reference for day sessions and the night shift. Change it when a decision changes.

## 1. Goals and milestones

Priority order: GTA V > Marvel's Wolverine > Demon's Souls. Every game must run from upstream main plus open PRs.
No long-lived private fork.

### GTA V (PPSA04264)

| Milestone | Measure | Now (2026-10-09) |
|---|---|---|
| G1. Correct rendering on the post-#639 base | gta-perf-3 replay PSNR equal to fidelity-main; live bank with no skipped draws | **done** on `night/merge` (`b19fe3e7`, `acb10a5d`) |
| G2. Merge at fidelity-main's speed | Lombank live, unprofiled, 60 s window | merge 23.5-25 fps against 27.8; replay per-draw CPU already equal |
| G3. Performance mode at 30 fps | Lombank and one street scene, worst frame under 33 ms | ~25-28 fps |
| G4. Performance mode at 60 fps | the same at 16.7 ms | far: committer ~25 us x 1343 draws = 34 ms of CPU per frame |
| G5. Fidelity mode with ray tracing at 30 fps locked | gta-fid-rtfix replay, then live | `gta-v/hw-rt` with HW RT: 307-450 ms per frame (texture budget raised by hand). The plain merge with software RT already does 133-216 ms (2026-10-09): port hw-rt onto the merge before any other RT work |

G4 needs a different shape of driver: the draw cost must go from ~25 us to ~10 us per draw, or the draws must be
spread over threads (the committer is the limit, see the 10-07 note on `APS5_PARALLEL_DRAWS`).

### Marvel's Wolverine (PPSA03671)

| Milestone | Measure | Now |
|---|---|---|
| W1. Main menu | the menu is shown and responds | pre-#639 `wolverine-capture`: INITIAL SETUP menu with correct text at 0.46 fps. On the #639 port `night/wolverine-merge`: dies at frame ~8 (heap corruption in the game's allocator, see NIGHT_SHIFT.md W) |
| W2. First gameplay load | in game | not started |

The port (10-09 evening) kept upstream's recompiler, driver, AMM, APR and AJM. Wolverine's GPU features are re-ported
onto #639 one commit each (NIGHT_SHIFT.md W3); `wolverine-capture` stays as the reference until they are.

### Demon's Souls (PPSA01342)

Parked. The menu runs at 35-46 fps; the open issue is the knight's lighting (G-buffer coverage). Pick it up only
when GTA V and Wolverine are blocked on something else.

## 2. Branches after upstream #639

- **`gta-v/fidelity-main`** (pre-#639): frozen. Only fixes needed to keep comparing against it. The two pending
  fixes (the `DrawRegisterKey` null `strcmp` guard and the teardown deadlock `6799740e`) go there once, so its replay
  and tests stay usable as the reference.
- **`night/merge` = `gta-v/fidelity-main-639`** (fidelity-main merged with upstream main incl. #639): the
  integration branch. All new GTA work lands here.
- **`night/perf`, `gta-v/hw-rt`, `night/wolverine`**: paused. Each is merged onto `night/merge` when its topic
  becomes the night's topic again (section 5), with its replay check.
- Upstream PRs take their changes from `night/merge` and are branched from a quiet main commit (see section 4).
- **Keeping up with upstream main:** small and often.
  - The night merges main into `night/merge` as its first task, and runs the suite. It merges only a **quiet** main
    commit: one followed by at least 2 hours without commits. Main moves all day, and a half-landed series is no base.
  - The morning's base replay is the check: frames and CPU against the day before.
  - The day session does not merge: a merge moves the baseline mid-investigation and can eat hours.
  - A large upstream rework (spotted during the morning PR round) gets a dedicated merge night.
  - The queue says "no merge tonight" when the day needs a stable baseline (a live-test day, an A/B in progress).
- One integration branch for every game; whatever the day's game is, it runs from `night/merge`.
- **`gta-v/testing`** (the testers' branch, pre-#639): retired once `night/merge` reaches its live frame rate (27.8
  fps in the bank against 23.5-25). No fixes go only there.
- **`testing`** replaces it: one testers' branch for every game. It is not a branch with its own commits: it points
  at a known-good `night/merge` commit and only moves by fast-forward, after the day checks. A commit qualifies when
  GTA V reaches in-game at its current fps, Wolverine reaches its current milestone, and the day checks show no
  regression. Testers never get `night/merge` itself (the night commits to it and merges main into it every night).
  Every tester build is a `night/merge` commit, so their reports map straight onto our history.
- **`night/wolverine-merge`** (the Wolverine port onto #639) merges back into `night/merge` as soon as it no longer
  breaks GTA V. It does not wait for Wolverine to fully work: its remaining work continues on `night/merge`.
- **main plus a PR stack** (e.g. `night/gta-main-stack`): a throwaway check, built only to show that our PRs alone
  are enough on main (upstream asks, or before opening a PR). Never maintained as a branch.

Decided on 2026-10-09: `night/merge` is the integration branch from now on; G2 (the live 10%) is treated as
ordinary perf work on it.

## 3. How we test

1. **The replay first.** Every GPU-driver change is checked on the gta-perf-3 capture (Performance) and, for RT,
   on gta-fid-rtfix:
   - frames: `--compare <capture>/live`; the PSNR must not drop, and `[gpu] skipped` / `pipelined commit failed`
     must be absent;
   - CPU: loops 4-5 of `--loop 6` with `APS5_PROFILE_DRAW=1` (frame ms, worker us per packet, committer us per draw).
   The replay runs without the user and without a game window.
2. **Live only to confirm.** Lombank, unprofiled, fps read from the window title over 60 s, after the first
   visit's compiles. One launch per batch of changes. A profiled run only when a number moves and we need to know
   why.
3. **Live-only differences are bugs in what the replay does not exercise:** shader registration and ABI hints,
   long-lived caches, uploads over minutes. Today's two live-only failures were both in registration-time
   preparation. A live run must be read for `pipelined commit failed` as well as `[gpu] skipped`.
4. The full ctest suite on the 5080 before a push to an integration branch. Known environment failures:
   `agc_driver_wave_allocation`, `agc_driver_wave_mask_shift` (NVIDIA), the two `null_texture_proof` teardown hangs.

## 4. Upstream PRs

As agreed on 2026-10-09 (memory `pr-triage-plan`):
- at most 5-6 ready PRs; drafts are parking;
- **answer reviews right away**, during the day: a reviewer waiting a day costs more than anything else;
- **rebase once, at the end of the day**, and only a PR that is both conflicting and active; never rebase to stay
  current;
- **open a new PR only when a slot frees up**, cut from the integration branch, one topic, with a test;
- never push unfinished work to a PR branch; experiments go to the private night/integration branches, which are
  pushed freely;
- no stacked chains; testers' fixes come as their own follow-up PRs.

Next PR candidates once `night/merge` is the base:
- the GTA perf switches as 2 PRs;
- the pipelined draw stack ported onto #639 (replaces drafts 1191-1202 and 1747);
- #423 reworked to runtime clip/cull enables.

Today's merge fixes (rect-list first-use preparation, rect fault slot, the null pixel program in the replay
registration) are upstream bugs too. Each goes upstream as a small PR with a test.

## 5. Night shift and day sessions

The PC is only on while the user is awake, so nothing runs on the 5080 before the day session starts. The night's
checks must therefore be cheap enough to run in the background during the session's first half hour.

### One topic per night

The night works on **one game, one branch, one capture**. It switches topic deliberately when a milestone is reached,
not every night.
- Three topics a night meant three builds and three kinds of day tests (2-3 hours of testing).
- It also meant three diverging branches to merge later, and ~3 hours of depth per topic.
- The fallback topic is used only when every task of the main topic is blocked.

| Period | Night topic | Branch | Day check |
|---|---|---|---|
| from 2026-10-09 | GTA V Performance: G2, then G3 | `night/merge` | `gta-perf-3` replay (~30 min) |
| after G3 | GTA V ray tracing: first hw-rt ported onto the merge (its software RT already beats hw-rt's frame), then the texture-table budget (`APS5_TEXTURE_TABLE_RESIDENT` did not fix it) | `night/merge-rt` | `gta-fid-rtfix` replay |
| fallback | Wolverine port onto #639 (AMM switch, crash reports, GPU features) | `night/wolverine-merge` | live checks only (no #639 capture yet) |

### Who does what

| | Night shift (cloud, no GPU, ~10 h) | Day session (5080, with the user) |
|---|---|---|
| Does | the topic's queue: ports, fixes, analysis, designs, implementations behind switches, CPU timing with the `APS5_NULL_SUBMIT` replay | runs the night's checks, decides, live checks, PRs, the next night's queue |
| Hands over | progress file + `docs/dev/perf/day-checks.sh` (one `check` line per change, at most 8, ~30 min in total) + a short "Live checks" list | results written back into the progress file; switches made default or deleted |
| Rules | no subagents; hard multi-hour tasks; push often; everything new behind a switch | a switch is decided within a day of its check |

### The day

| When | What | Time |
|---|---|---|
| Start | Start `day-checks.sh` in the background. PR round meanwhile: new comments, merges, answer reviews. Then read the check summary and decide each switch | ~30-45 min |
| Main block | The day's goal. Iterate on the replay; long test batches run in the background while other work goes on | most of the day |
| One live slot | Every live check that needs the user, batched into one launch at a time that suits them. Live checks without driving use `gta-to-world.ps1` and need nobody | ~15-30 min |
| End | PR maintenance (section 4), the next night's queue with its day checks, push the integration and night branches | ~30 min |

### Test cost

- Warm shader caches per branch and capture; cold-cache runs only when a test is about compile time.
- One base run per morning; every check compares its frames against it.
- Switches do not pile up: measured means default or deleted.

## 6. Day-session test backlog (2026-10-09)

What the night of 10-09 asked for, with today's results.

| # | Branch | Test | Who | Status |
|---|---|---|---|---|
| 1 | merge | gta-perf-3 replay frames | me | **done**: 20.2/21.0 dB (fidelity-main equal) after `b19fe3e7` |
| 2 | merge | live menu and bank | user | **done**: menu 60 fps; bank renders correctly after `acb10a5d`, 23.5-25 fps |
| 3 | merge | tolerated dispatch `0x242d49600` dispatches, no `skipped dispatch` | me | **done**: it dispatches in the replay; no `skipped dispatch` in the replay or live |
| 4 | merge | template proofs `APS5_NULL_TEXTURE_PROOF=1 APS5_DEPTH_TEXTURE_PROOF=1`: frames equal, no "no record" walks | me (replay) | **done**: frames identical, "no record" walks 386 -> 0; now default on |
| 5 | merge | `APS5_FAST_STRUCTURIZER=1`: registration time (242 s -> 36 s expected), frames equal | me (replay + automated time to menu) | **done**: frames identical, setup 166 s -> 21 s; now default on |
| 6 | merge | `APS5_SPIRV_SPECIALIZE=light`, `APS5_SPIRV_VALIDATION=prepared`: `[specialize]` totals, frames equal | me (replay, cold cache) | **done**: no effect (one module specialized); switches deleted |
| 7 | merge | first-visit hitches with item 6's switches | user (drive into a new area) | dropped with item 6 |
| 8 | merge | `APS5_BINDLESS_SLOTS` 16 / 64 / 128 on the RT capture: missing textures | me (replay) | **done**: the merge runs the RT capture (133-216 ms, software RT); 64/128 change nothing; keep 16 |
| 9 | merge | the live 10% gap: one profiled fidelity-main live run in the bank, diffed against today's merge log | user (one launch) | open |
| 10 | hw-rt | gta-fid-rtfix with `APS5_TEXTURE_TABLE_RESIDENT=1`: one `[resources] images` line instead of 36, frame near 363 ms | me (replay) | **fails**: ~4.4 s per frame, 19 rebuilds; 4436 bindings over 4297 surfaces |
| 11 | hw-rt | `APS5_BVH_EXACT=1` (HW RT off): reflections against the default, time within noise | me (replay PNGs) | dropped: software fallback only |
| 12 | hw-rt | `APS5_BVH_NODE_BUDGET=8192` (no change) and `=64` (distant geometry lost) | me (replay) | dropped: software fallback only |
| 13 | wolverine | menu load with C1 + C2, then each alone: VRAM totals at the wall, frame past ~881 | me (automated launch) | paused with Wolverine |
| 14 | wolverine | the menu's frames with both switches: wrong or stale textures | me (screenshots) + user to judge | paused with Wolverine |
| 15 | wolverine | `APS5_DUMP_SHADERS=1` at the menu load (C3) and a dump of dispatch `11d049aa00` (C4) | me (automated launch) | paused; both dumps need code first (graphics stages are not dumped, no table dump exists) |
| 16 | fidelity-main | push the strcmp guard and `6799740e` | me after approval | open |

Items 4-6, 8 and 10-15 need no game navigation from the user. Items 7 and 9 need one launch each.

## 7. Open engineering work (not tests)

- The merge took upstream's `ResourceMaterializer.cpp` and lost 732 lines of fidelity-main: the SRT evaluation
  memo, its timers, the bindless table code. Other files resolved to upstream's version may have lost work the same
  way. A function-level diff against fidelity-main is the next night task for `night/merge`.
- `splitSharedMergeBlocks` recomputes per split (registration cost).
- LS-stage snapshots without register 329 prepare at first use; upstream's registration should handle them.
- Shaders whose sampler pairs exceed the runtime heap capacity are left to first use (16 live).
- G4: a design for spreading draw preparation over threads with an ordered emit (the split committer of B1).
