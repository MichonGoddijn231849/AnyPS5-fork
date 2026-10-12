# Night shift: the queue (night of 2026-10-10 to 10-11)

You are an unattended Claude Code session in a Linux container **without a GPU**, in a **private** repository.
This file is the queue for the whole night. Read it fully before you start, then read `docs/dev/DEV_PLAN.md`
(the plan this queue follows) and `docs/dev/perf/MERGE_PROGRESS.md` (last night's and today's results).

## How the nights work from now on

- **One topic per night, on one branch, then the fallback.** Tonight: **performance only, GTA V on `night/merge`** (P below).
  **When every P task is done or blocked, continue with W** (Wolverine on `night/wolverine-merge`) for the rest of
  the night. Do not stop while budget remains. Do not touch `night/perf`, `gta-v/hw-rt` or `night/wolverine`: they
  are paused.
- **Work the whole night.** The tasks are big on purpose; nobody expects all of them to be finished. A task that is
  half done, committed and written up is a good result. When a task is blocked, write down exactly where it stands,
  commit, push, and go on to the next one.
- **No subagents.** Do everything yourself in this session (no Agent/Task tool, no parallel workers).
- **Go deep, not wide.** One structural change that removes a whole cost beats five memo switches.
- **Commit and push often**, after every meaningful step with its tests passing. Push only `night/merge` and
  `night/wolverine-merge`. Never push anywhere else, never open PRs, never touch the upstream remote.
- New behaviour goes behind an `APS5_*` switch that is **off by default**; bug fixes need no switch. Name the switch
  in the commit and in the progress file. When a switch is clearly a win on the CPU replay and changes no output, say
  so in the progress file: the day session makes it the default or deletes it within a day.
- Follow `docs/dev/CONVENTIONS.md`. Write code with the editor tools, not shell heredocs (a `\n` in a C string
  becomes a real newline there).
- Setup: `~/hwrt-env.sh`, build directory `build-merge`, configured with `-DBUILD_TESTING=ON`. The capture
  `testdata/gta-perf-3` is on this branch (Git LFS: `git lfs pull --include="testdata/gta-perf-3/*"`, then `unxz -k`).
- The shader disk cache key guard: a recompiler symbol that driver code newly calls must be added to
  `shaderCacheReviewedNames` in `core/shader/recompiler/CMakeLists.txt` (the build fails until it is).

## What you can and cannot check without a GPU

- Correctness: the `agc_*` suite on lavapipe (`vopc_compare` fails there, lavapipe's bug). Recompiled guest shaders
  do not run correctly on lavapipe; check SPIR-V statically and with hand-written kernels
  (`ps5run/tools/rdna2_encode_example.py` style encodings).
- CPU timing: `tools/perf/replay-bench.sh` replays `gta-perf-3` with `APS5_NULL_SUBMIT=1`. A/B on the same machine
  only, interleaved runs; report microseconds per draw (committer) and per packet (worker) with the spread.
- Frames: not here. Every change that could alter output gets a day check (below).

## Day checks: write them as you go

For every change the day session must look at, append one line to `docs/dev/perf/day-checks.sh` (the runner is
already there, read its header; it has no checks left today):

```
check <name> "<what passes>" "<log line pattern to show>" APS5_SWITCH=1 ...
```

**Keep the list to 8 checks at most.** Combine switches that cannot interact; drop checks a later commit made moot.
Bug fixes without a switch need no check: the base run covers them. A check that needs the live game goes into the
progress file's "Live checks" list instead, with the exact log lines to read; keep that list short too.

## Where things stand (day session of 2026-10-10)

- `night/merge` 8bc7f2b6: the split committer is on by default (`APS5_SPLIT_COMMIT=3`; day checks: same frames,
  frame ms 4-14% and the committer 4-10% below base). `APS5_DRAW_INPUT_MEMO` was 2-3x slower and is removed.
- Last night's P3 (in `MERGE_PROGRESS.md`): the live fast clears are GPU clears already (`vkCmdClearColorImage`
  from `StorageTexture::upload`'s clear path), but live GTA re-clears ~15x as many surfaces per frame as the capture
  (~4.6 per frame, ~1110 per 10 s), and each costs the CPU a barrier pair, a collect over the whole surface and,
  without an active recorder, a submit and wait of its own. `e8689aff` removed #2096's per-clear guest snapshot of
  watched memory.
- The pipeline cache (`PipelineCache.cpp`) is saved every `SaveIntervalSeconds` = 10 s. On a cold cache (~350 MB)
  the day saw 17 saves in one run on top of the compile stutter; a warm run made 2.
- Wolverine on `night/wolverine-merge` (see W): menu, settings, title screen and the first cutscene render.
- Upstream main merged ~25 PRs between 00:00 and 00:35 on 10-11 (#1961 imports GPU-mapped memory at map time, #2233
  pooled buffers at exact size, #1940 aperture sources, #1873 queue thread priority, ...). If it is quiet by the time
  you start (P0's 2-hour rule), that merge is big; expect conflicts in `GuestBufferMemory.cpp` and `Resources.cpp`.
- Known on `night/merge` (not tonight's work, for the day session): `agc_driver_wave_allocation` and
  `agc_driver_wave_mask_shift` fail (outputs never written; both use a structured buffer V# with stride 0) while they
  pass on main and on 10-08 builds.

## P. GTA V on `night/merge`

0. **Upstream main first.** `git fetch upstream main` (read-only; never push there; if the remote is missing:
   `git remote add upstream https://github.com/boykopovar/AnyPS5.git`). Merge only a **quiet** main: the newest
   commit followed by at least 2 hours without commits. If that is the commit already merged, skip and say so.
   Otherwise merge it into `night/merge`, resolve the conflicts, run the suite, write the merge up (commits merged,
   conflicts, anything of ours that changed). If main has a large rework of the driver or recompiler, do the merge as
   the whole night's work and say so.

   **How to resolve conflicts (`night/merge` is the branch every game runs from; a fix lost here costs a day of
   re-debugging a game):**
   - **Keep both behaviours.** A conflict means both sides changed the same lines, not that one side is wrong. Write
     the result so main's change and ours both hold.
   - **Never take a side wholesale** (no `-X ours` / `-X theirs`, no `git checkout --ours/--theirs` of a whole file,
     no "take main's file and re-port ours later"). Resolve hunk by hunk. That is how the Wolverine port lost fixes
     on 10-09 (`f1e595f39` took #639's driver and recompiler whole; the fixes listed from memory were incomplete).
   - **When main's rework has to win** (it restructured the code our change lived in), re-apply our change on top of
     main's code in the same merge, not later. Find what our side was for first: `git log -L` / `git blame` on our
     side of the hunk gives the commit and its message; read it.
   - **Our tests must keep passing.** A test of ours that fails after the merge is a lost fix until shown otherwise;
     do not change a test's expectation to make the merge pass without writing down why.
   - **Write up every conflict** in `MERGE_PROGRESS.md` under the merge: file, which side's code survived, the commit
     ours came from, and whether ours was kept, re-applied on main's code, or dropped (with the reason). Dropped ones
     also go to the top of the morning hand-off so the day session checks the game that needed them.
1. **Pipeline-cache saves (small, do it first).** A 350 MB `vkGetPipelineCacheData` plus file write every 10 s
   while new pipelines compile is a stutter source of its own.
   - Find which thread saves, how long one save takes at that size (the `[pipeline-cache] saved ... in X ms` line
     under profiling), and whether the save holds a lock the compile or draw path needs.
   - Change it: save only when the cache grew by a meaningful amount since the last save, at most every 60 s, at the
     end of a compile burst (no new pipelines for a few seconds) rather than in the middle of one, and always at
     teardown. Off the game's and the committer's threads, holding no lock they take. Write the file to a temporary
     name and rename it, so a crash mid-save keeps the old cache.
   - A test with a fake clock or injected save hook: a burst of pipeline creations saves once after it, not every
     interval; teardown saves; an unchanged cache is not saved again.
   - No switch needed if the behaviour only moves saves (say so); keep `SaveIntervalSeconds` as the minimum spacing.
2. **DCC fast clears: make the per-clear CPU cost small.** The clears run on the GPU; their CPU side does not scale
   with ~1110 clears per 10 s live. Read `StorageTexture::upload`'s clear path, `IsDccClear`,
   `StorageClearAvailable` and the `clears N recorded / M waited` counters in `Texture.cpp`, then:
   - (a) **No waited clears.** A clear with no active recorder submits and waits on its own (a GPU round trip).
     Record it into the next batch (or a clear queue the next batch drains first) instead, with the ordering rules
     of the pipelined path held: the clear must land before any recorded use of the surface and after earlier writes
     to it. A test that clears with no recorder active and checks the next draw sees the cleared value with no wait.
   - (b) **No whole-surface collect per clear.** A fast clear only needs to know the DCC keys still say "cleared"
     and the surface's memory has no newer CPU writes; find what the collect over the whole surface is for and
     whether the keys' pages (or the write watch's per-range serial) are enough. A large render target re-cleared
     every frame should cost microseconds, not a walk.
   - (c) **The clear value from the guest's fast-clear registers on the GPU.** If the clear colour is read on the CPU
     from the CB registers or the clear-colour words in guest memory each time, cache it per surface and generation.
   - Measure each with `replay-bench.sh` (the capture clears 0.3 per frame, so the CPU numbers move little; count
     the per-clear cost in a micro test instead: N clears of one 4K target, us per clear, before/after). Add one
     combined day check if any step has a switch, and a live check: the `[texture] ... clears R recorded / W waited`
     line (W must be 0) and the `images: upload clear` row of the `[indirect]` line, in the bank at 60 s.
3. **The live-only gap, from last night's instruments.** If the day session added a live profiled run's lines to
   `MERGE_PROGRESS.md` ("Live checks (night of 10-10)" item 1), act on what they show: preparation during play
   (first-use preparations or registrations above ~100 ms per 10 s), the prepared-entry scan (invocations above
   ~3 us or missing their first entry), or the clears (task 2). If there are no such lines, skip this task and say so.
4. **Per-draw CPU on the split committer.** The committer's large phases are record (~10.5 us), lookup (~8.4 us) and
   vertex (~7 us); ~13% of the sampled profile is malloc/free, much of it `CopyDrawInput`'s buffers. Pool or reuse
   those buffers per committer thread (structural: no per-draw heap allocation on the draw path), measure with
   `replay-bench.sh` (`--sample` for the profile) against base, interleaved.

## W. Wolverine on `night/wolverine-merge` (when every P task is done or blocked)

Check out `night/wolverine-merge` from `origin` (260f6499e or newer). Read `docs/dev/wolverine/PORT_INVENTORY.md`
(every old-branch commit and what became of it; the old branch is gone, its code is the tag `archive/wolverine-capture`)
and `docs/dev/wolverine/SWITCHES.md` (each switch's kind and path to a PR) first. Write everything up in
`docs/dev/wolverine/PORT_PROGRESS.md` (create it).

State after the late session of 10-10 (live and with the cutscene capture on the day machine, RTX 5080): intros
~30 fps; the INITIAL SETUP menu, settings, title screen, save slots and the first cutscene ("30 YEARS AGO") render, at
~0.3-0.7 fps. Still wrong in the cutscene: a ~1024 x 300 rectangle in the top left of the scene color with other sky
content, and stair-stepped edges where terrain meets sky (16 px tiles: the tiles that mix sky and geometry are
shaded by the complex-tile lighting compute 0x11d049aa00, which never ran).

Done in that session (all on the branch, see the commit messages): sampler heap 64 pairs (runtime ABI 15), a T# with
no base address is the null descriptor under `APS5_NULL_UNRESOLVED_IMAGES`, buffer views past their owner clamped,
**subroutine and function-table calls** (`APS5_SUBROUTINE_CALLS=1`, compute only; port of the old pass plus a
table dispatcher; test `agc_driver_subroutine_calls`), the SRT plan builder walks with an explicit stack, runtime-
format buffer loads read once per layout and select once (`APS5_FORMAT_SWITCH_PER_FORMAT=1` = old), pipelines of
modules past `APS5_UNOPTIMIZED_PIPELINE_WORDS` (1M) are created with `DISABLE_OPTIMIZATION`.

Tasks, in order (no GPU here: the recompiler offline, hand-written kernels on lavapipe):
1. **Make 0x11d049aa00 compilable (performance of the recompiler's output).** With the calls it recompiles, but to
   1.64M SPIR-V words, and NVIDIA's pipeline compile of it did not finish in 14 minutes (memory flat at ~17 GiB).
   `testdata/wolverine-requests/` has the request and two scripts (README there): `main` ~1.40M words, plus
   `load_formatted_gpu_buffer_x3` / `_x4` (~33-34k words each) called 78 and 48 times, which drivers inline (NVIDIA
   ignores `DontInline`): ~5.6M words compiled. For comparison `shader_11d0376e00.req` (2.77M words, one ~8k helper
   called 192 times) compiles in a few minutes. Goal: under ~1.5M compiled words (main + each helper x its calls),
   with the same results.
   - The format helper: one read stage shared by every layout (the element's dwords read once, each layout only
     extracts and converts), instead of one read per layout group (`formatLoadSwitch` in `SpirvRuntimeBuffer.cpp`).
     `agc_driver_buffer_format` and the other buffer format tests cover the runtime-format path; add a row per layout
     group you merge if a group has none.
   - `main`: find what its 1.4M words are (60k blocks, 12k calls, mostly Select/Bitcast/Constant/LogicalAnd/BitwiseAnd
     per the histogram); per-instruction expansions repeated across the 11 inlined callees are the likely target.
   - Report words per step (`spvcalls.py`) in the progress file; a live check for the day session: the replay of
     `ps5run/captures/wolverine-cutscene` reaching `[replay] loop 0` with 0x11d049aa00 not skipped, and its frame.
2. **Bindless storage image tables** (0x11d0395500, `bindless storage image tables are unsupported`): a storage
   variant of the wide tables (`WideImages`-like storage arrays with the same key map), with a test.
3. **`v_readlane_b32` of lanes 31 and 63 after a wave64 OR-scan in a graphics stage on 32-lane subgroups** (draw
   shader 0x11d00c4c00, stage 2): `MatchHalfWaveScan` (SpirvAluEmitterMath.cpp) does not take this scan: some of its
   DPP updates take their old value from the other word's chain. Print the DPP flags of that chain first (row masks,
   bound_ctrl, controls); if the lanes the old value fills are overwritten or masked, extend the matcher; otherwise
   design the cross-half exchange. With a test.
4. **Stencil export** (DB_SHADER_CONTROL 0x43, one draw into 0x113f230000): bit 1 (stencil test value export) is
   refused everywhere. MRTZ export component 1 to `FragStencilRefEXT` (`VK_EXT_shader_stencil_export`, capability
   `StencilExportEXT`), the pixel input info and cache key, the request serializer (next version), the state mask in
   `State.cpp`. With a test.
5. **Small refusals**, one commit each with a test: `null or misaligned address` (eboot compute 0x7ff6af15db00),
   `buffer descriptor uses an unsupported type` (a DRAW_INDEX_2 draw into 0x3c67b0000).
6. **Keep it merged.** Merge `night/merge` into `night/wolverine-merge` (after P's commits; it is 133+ commits
   behind), resolve, run the suite. List in the progress file every default-on change of this branch that GTA V has
   not been checked with yet (SWITCHES.md has the list) for the day session.

Day checks for W go into `PORT_PROGRESS.md`'s "Live checks" (the capture `ps5run/captures/wolverine-cutscene` replays
the cutscene offline on the day machine), not into `day-checks.sh`.

## Morning hand-off

At the end, in `docs/dev/perf/MERGE_PROGRESS.md`, under "Night 2026-10-11":
- what is done: commit, switch, what the test proves, what was measured here;
- the day checks you added to `day-checks.sh`, and the live checks;
- what is left.

Push `night/merge` (and `night/wolverine-merge` with its `PORT_PROGRESS.md` if W ran).
