# night/perf progress

## Night 2026-10-08

Base: `a1b6431c` (`gta-v/fidelity-main` `f335ca1b` + the `gta-perf-3` capture). Built in `build-perf` on lavapipe.
The full ctest suite (300 tests) passed at the start of the night.

### Result: the recommended set against the base

The night's switches together, `APS5_RETIRE_ON_WORKER=1 APS5_BATCH_SUBMITS=1 APS5_PIPELINE_HYSTERESIS=1
APS5_TARGET_KEY_PROOF=1`, against the same build without them. Four interleaved runs each, 12 loops
(`tools/perf/replay-bench.sh`):

| Run | Committer us per draw | Worker us per packet | Frame CPU (committer / worker) | Batches per frame |
|---|---|---|---|---|
| base | 63.3 / 61.7 / 61.8 / 62.2 (62.3) | 62.0 / 61.6 / 60.9 / 61.7 (61.5) | 83.7 / 82.7 ms | 63.8 |
| set | 46.1 / 45.1 / 46.9 / 45.2 (45.8) | 45.9 / 45.4 / 46.3 / 45.1 (45.7) | 61.5 / 61.4 ms | 41.2 |

-26% on both threads, well outside the ~10% noise. Adding `APS5_COLLECT_GRANULE_KIB=64` (committed after this
table) takes the committer to ~40.5 us per draw (two runs), -35% from the base. A 40-loop run with all five
switches: committer 41.0 (+-1.8), worker 43.0 us; no errors, no blocked queue. Later, with `APS5_DRAW_KEY_MEMO=verify` and
`APS5_COMMITTER_SPIN_US=50` added (seven switches, 40 loops): worker 37.5 us per packet, committer 44.7 (the spin
included); no errors and no verify abort. The replay's resident memory stays at
10.56 GB (+4 MB over the 40 loops), so the retired commits are not piling up. The base already includes the rect-list memo (B7) and the
allocation cuts (B6). At the start of the night (B0, before either) the replay read ~84 us per committed draw and
~92 us per packet.

### Summary: commits, switches, measurements

| Commit | Item | Switch | Measured here (replay, `tools/perf/replay-bench.sh`) |
|---|---|---|---|
| `a0baa9f8` the replay as a CPU instrument | B0 | `APS5_NULL_SUBMIT=1` (the replay's mode) | baseline: 1343 draws per frame, committer ~84 us per committed draw, worker ~92 us per packet (noise ~10% run to run) |
| `5e5aa05b` `replay-bench.sh` | B0 | - | one line per run |
| `f4507cbd` B1 state table and design | B1 | (`APS5_SPLIT_COMMIT` designed, not implemented) | - |
| `e5297bb2` batched submits and a submit thread | B2 | `APS5_BATCH_SUBMITS=1`; `APS5_TRACE_SUBMITS=1` (submit sites) | 68 -> 52 recorded batches per frame (24 with `APS5_LABEL_FLUSH_US=4000`); committer change inside the noise |
| `ab5f4e56` fewer committer allocations | B6 | none (same results); `APS5_COUNT_ALLOCS=1` (the replay's allocation counter) | 70.7 -> 40.7 heap allocations per committed draw (8.8 -> 6.8 KB) |
| `81d3ffa9` worker allocation sites | B6 | `APS5_COUNT_ALLOCS=worker` | worker: ~225 allocations per draw packet, by site |
| `4dcb45df` rect-list shaders memoized | B7 | none (same modules); `APS5_NO_RECT_LIST_MEMO=1` builds every time | worker 79.7 / 77.9 -> 59.4 / 62.5 us per packet (-22%), its frame CPU 105-107 -> 80-84 ms; committer unchanged |
| `d494501f` CPU profile of the driver threads | B8 (instrument) | `APS5_CPU_SAMPLE=1` (+ `tools/perf/cpu-profile.py`) | - |
| `bbc54b47` committed draws freed by the worker | B8 | `APS5_RETIRE_ON_WORKER=1` | committer 61.7 / 62.7 -> 50.3 / 50.0 us per committed draw (-19%), worker 61.3 / 62.6 -> 48.0 / 47.7 us per packet (-23%) |
| `47e17261` wake the worker at half depth | B8 | `APS5_PIPELINE_HYSTERESIS=1` | with retire + batching: the committer's wakes 4.2% -> 1.2% of its CPU, worker samples -7%; us per draw inside the noise |
| `7f7e809b` render target DCC keys scanned once per change | B8 | `APS5_TARGET_KEY_PROOF=1` | with the three above: `AllKeysEqual` (3.3% of the committer) leaves its profile, committer samples -3%; us per draw inside the noise |
| `bdf5c1ee` memoized collects walk the whole granule | B8 | `APS5_COLLECT_GRANULE_KIB=64` | with the four above: write-watch walks -30%, committer 45.5 / 44.2 -> 40.8 / 40.2 us per draw (-9%) |
| `ebfbf66e` the draw key's context-bank hash kept | B8 (worker) | `APS5_DRAW_KEY_MEMO=1` (`verify` checks) | with the five above, three interleaved runs each: 64% of draws reuse it; worker 42.0 / 42.8 / 42.6 -> 40.1 / 42.1 / 40.3 us per packet (~-4%, inside the noise); its key/lookup phase 6.8 / 6.9 / 6.6 -> 6.2 / 6.6 / 5.9 us; `drawRegisterKey` 4.8% -> 3.0% of the worker's samples; committer unchanged |
| `7307327c` the committer polls before it blocks | B8 (worker) | `APS5_COMMITTER_SPIN_US=50` | with the six above, two interleaved runs each: frame wall 75.8 / 77.2 -> 71.3 / 71.7 ms (-6.5%); worker 40.5 / 40.3 -> 35.9 / 36.6 us per packet (-10%, the Enqueue's futex wake gone); commit work 32.5 / 32.6 -> 32.1 / 31.6 us; the committer's CPU +3.8 us per draw (the spin). 20 us: 73.9 / 75.9 ms, worker 37.2 / 37.7 |
| `af18ae84` a fresh draw entry's vectors reserved | B8 (worker) | none (same contents, fewer allocations) | with the seven switches, one sampled run each: `cacheDrawStages` 6.4% -> 4.3% of the worker's samples (its `operator new` 9 -> 4 samples); worker 37.6 -> 35.8 us per packet in that pair (one run, inside the noise) |
| `b29d8b80` the materializer's SRT evaluation memoized | B4a | `APS5_SRT_EVAL_MEMO=1` (`verify` checks) | with the seven switches, four interleaved runs each: ~6350 misses (the first frame), then every call a hit; the worker's capture phase 8.8 / 10.7 / 9.2 / 8.9 -> 7.6 / 12.8 / 8.7 / 8.1 us per packet (the 10.7 / 12.8 pair was a noisy run); worker frame CPU 51.0 / 57.0 / 51.0 / 52.0 -> 48.3 / 53.0 / 50.4 / 52.2 ms; total worker us per packet inside the noise |
| not committed (patch: `docs/dev/perf/attempts/b4a-allocation-free-key-bulk-reread.patch`): the memo's key without allocations and the bulk re-read | B4a | `APS5_SRT_EVAL_MEMO=1` (reverted) | six interleaved pairs, committed memo against the new code, loops 3-11: worker 50.0 -> 49.4 us per packet, frame 107.8 -> 106.7 ms, capture 10.8 -> 10.6 us, SRT evaluate per capture 4.75 -> 4.9 us (all inside the noise); `=verify` clean; suite 305/305; negative control fails as intended; not kept |
| `ee358160` agc_driver_pipelined_dispatch, drains counted per opcode | B5 | none (test and `DrainRequestsByOpcode` only) | a compute dispatch after the immediate fill of its input, in one DCB, five rounds: each round's output is checked after the wait; with the switch off, every dispatch drains once |
| `c2e1361e` dispatch-cache recipe hits as commit items | B5 | `APS5_PIPELINE_DISPATCH=1` (with `APS5_PIPELINED_DRAWS=1`) | replay, two interleaved pairs, 12 loops (final code): DISPATCH_DIRECT drains per 10 s window 203-248 (off) -> 0 after the first window (on); frame ms 105.1 / 117.3 -> 104.0 / 111.3 (inside the noise); full suite 305/305 with the switch off and on; negative control (read-side overlap check skipped) did not make the test fail |
| `3cfc922b` pipelined dispatch test: a scalar read of the input | B5 | none (test) | the read-side rule is exercised: the entry covers the input (one 4-byte region), with the switch on each round drains once (`reads`) and the output is checked; negative control (overlap check forced off, temporary): `agc_driver_pipelined_dispatch_item` fails at scalar round 1 with a stale input (0x13572469 for 0x2468ace1 expected); the switch-off test passes; full suite 305 of 305 with the switch off and on |

- **Not done.**
  - **B1** (the committer split) is designed; the state table is below.
    - *First step tried, not kept: the pipeline lookup on the worker* (`APS5_PREPARE_ON_WORKER`; the patch is in
      `docs/dev/perf/attempts/b1-prepare-on-worker.patch`). The worker peeks the fragment-output memo, the shared
      resource cache and the pipeline store, and hands a prepared pipeline to the commit closure. The committer uses
      it only when the template it resolved is the same object and the state, shaders, fragment outputs and vertex
      input are the ones it was prepared from.
    - It is correct: in verify mode (the committer's own lookup on every reuse, abort on a different pipeline) the
      replay reused 37000 pipelines with no mismatch, and 99% of draws were prepared.
    - It is not faster. Three interleaved pairs with the seven switches (in a fresh container, slower than the
      earlier runs): worker 49.1 / 51.8 / 54.1 -> 54.8 / 57.8 / 56.8 us per packet, committer 58.5 / 62.3 / 66.0 ->
      60.4 / 62.7 / 61.8, frame 100.3 / 109.4 / 114.3 -> 107.0 / 112.8 / 107.6 ms. The worker recomputes the
      resource key and the pipeline key (the expensive part), while the committer still computes its own resource
      key to resolve the template, so only the ~2 us `CachedPipeline` call moves. A real gain needs the template
      resolution itself (key, find, revalidate) to move with it, which is the full split.
    - *Second step tried, not kept: the resource key and the template find on the worker too* (same switch; the
      patch file now holds this step as well). The worker computes `DrawResourceKey` and peeks the template with
      `ResourceCache::Peek`, which also records the cache's generation and the entry's LRU position. The committer
      uses the worker's key when the device, the `State` object, the compiled shaders and (under
      `APS5_NO_DRAW_KEY_TRIM=1`) the index range are the same. It takes the template by `TouchPeeked`, which moves the
      entry to the LRU front as `Find` does, while no insert, remove or clear has happened since (the generation).
      Otherwise it calls `Find`. Revalidate, the moved buffers and images, the aliases and any build stay on the
      committer. The `[prepare]` line now counts key and find reuse.
    - Verify (`=verify`, the replay with the seven switches, 12 loops): no abort. Over the three 10 s windows,
      39373 keys were reused and checked against the committer's own key, 38653 finds reused and checked against
      `Find`, 38931 pipelines reused and checked, 720 finds fell back to `Find` (the generation had moved), 434 draws
      had a different template on the committer (a fresh build, or a template the committer removed), and 0 mismatches. Full suite: 305/305 with the switch
      off, `=1` and `=verify`.
    - Samples (`--sample 15`, one run each, off and on; the committer's inclusive share): `resolveDrawResources`
      19.6% -> 17.0%, `DrawResourceKey` 1.9% -> 0.1%, `ResourceCache::Find` 1.0% -> not in the list, `CachedPipeline`
      1.8% -> not in the list, `TouchPeeked` 0.1%. Revalidate (7-9%) and the record stay as they were.
    - Pairs (four interleaved pairs, 12 loops, means of loops 3-11; off / on, in run order; pairs 2 and 4 ran on
      first): worker us per packet 55.1 / 51.8 / 53.1 / 56.3 -> 61.2 / 59.7 / 61.1 / 62.5 (mean 54.1 -> 61.1);
      committer us per draw 66.7 / 62.4 / 64.6 / 67.2 -> 66.4 / 64.5 / 65.1 / 68.1 (mean 65.2 -> 66.0); frame ms
      116.3 / 108.2 / 115.9 / 117.7 -> 120.1 / 116.2 / 116.0 / 123.7 (mean 114.5 -> 119.0). The worker is slower in
      all four pairs. One `=1` run (pair 4) failed at start with a replay memory-collision message (a captured image
      range against the process's own memory, an address-layout effect) before any draw; the rerun passed, and the
      message appears in no other run.
    - Not kept. The key and the find leave the committer's profile as intended, but its us per draw does not move:
      it is inside the noise and slightly higher on average. The worker now pays for the key, the template peek and
      the pipeline peek on every draw, about 7 us per packet more here, which is more than the committer saves.
      The code is reverted; the patch file keeps it for the day machine. What would make it a gain: a worker that
      can afford the extra key work (for example a key computed once per content and reused across draws), or
      revalidate and the record moving off the committer, which this step does not touch.
  - **B3.** Template misses are 0.8% in the replay, already under the 2% target. A hit's bindings cost ~7.5 us here
    (~3 live).
  - **Draw key memo** (`ebfbf66e`, `APS5_DRAW_KEY_MEMO=1`, a small piece of the "register hash" below). `Registers`
    now has a version that changes on every access that can write (non-const `operator[]` and `at()` too) and on
    every copy. The draw key hashes the context ranges first (a `static_assert` keeps them first), so the FNV state
    after them is kept per thread with the context bank's version; the keys are bit-identical. `verify` hashes
    anyway and aborts on a difference. The replay ran clean with it (12 loops, and 40 with all seven switches); none
    of the eight draw-related ctest tests checked with a breakpoint (`agc_driver`, `_draw_write_ranges`,
    `_pipelined_dma`, `_recorder`, `_graphics`, `guest_indexed_draw`, `guest_rendering_wait`,
    `linear_display_buffer`) reaches `drawRegisterKey`, so the replay is the evidence. The
    `Registers` version is always kept: the suite passes with and without the switch, apart from the two known
    lavapipe failures.
    `agc_driver_pm4` checks the version rules.
  - **Committer spin** (`7307327c`, `APS5_COMMITTER_SPIN_US=<n>`, off by default). The worker's `Enqueue` already
    wakes the committer only when it sleeps, but with the two threads balanced it often does: the profile showed
    `futex_wake` at 5-7% of the worker. An idle committer now polls the pipeline's outstanding count (with `pause`)
    for up to n us before it blocks, so the next item usually finds it awake. It trades a core's idle time for
    latency. `agc_driver_pipelined_dma_spin` (new ctest) runs the pipelined commit items with a 50 us spin; that test
    and `agc_driver` reach the spin (breakpoint). The spin is bounded by the committer's idle time: at worst its core stays busy through the frame's draw
    phase. Here it added ~5 ms of committer CPU per frame (53 -> 58 ms). On the day machine, check both the frame time
    and the CPU use.
  - **Profile with all seven switches** (12 loops, `--sample`). The worker has 390 samples:
    - heap calls (`_int_free`, `malloc`, `operator new`, `_int_malloc`, `free`, `malloc_consolidate`) are ~15%. The
      frees are mostly the retired commits, which `APS5_RETIRE_ON_WORKER` moved here (in `Enqueue`). The
      allocations are spread over `drawTail`, `cacheDrawStages` and `DecodeVertexStageInfo`, with no single site;
    - the SRT evaluation (`EvaluateRuntimeSources`, B4a's target) is 15% inclusive, `compileDrawStage` 33%;
    - `drawRegisterKey` is 2.6%, `futex_wake` 2.1% (it was 5-7% before the spin).

    The committer has 425 samples: the lavapipe `ioctl` 5.4%, `memcpy` 4.9%, `PrepareDrawBindings` 15% inclusive.
    `steady_clock::now` is 12.5% inclusive on the committer, partly the spin loop (it reads the clock every
    iteration; reading it every few dozen pauses would cut that, idle time either way). Both threads spend 5-8% of
    self samples in the vDSO clock, mostly the replay's phase timers (`APS5_PROFILE_DRAW`): a run without them does
    not pay that.
  - *Tried, reverted:* freeing the retired commits on a third (reaper) thread instead of the worker
    (`APS5_RETIRE_REAPER`). With the seven switches, two runs each: worker 37.9 / 37.5 -> 60.7 / 61.8 us per packet,
    committer 45.0 / 44.9 -> 62.4 / 63.1. Much worse. The commits' memory (and `ShaderMemory`'s per-thread page pool)
    was allocated on the worker; freed elsewhere, it no longer comes back to the worker's caches, which then
    allocate fresh. Freeing on the allocating thread (`APS5_RETIRE_ON_WORKER`) is the right place.
  - **B4a.** Done as a memo (`b29d8b80`, `APS5_SRT_EVAL_MEMO=1`), not as a compiled walk.
    - *What it does.* `materializeSnapshot` (`ResourceMaterializer.cpp`) keeps a per-thread map, bounded at 16384
      entries (cleared when full), from (plan address, user data, shader base, the two reader functions, the
      materialization sources, the clean flat slots, two plan sizes) to the evaluation's results, flat SRT and active
      sources. On a miss the evaluation runs through wrapping readers that record every word read (reader, address,
      success, value) in order. On a hit those words are re-read, in the same order, through the current call's
      readers (so `ShaderMemory`'s page fills and snapshot lookups still happen); the stored result is used only if
      every read returns the same success and value, otherwise the evaluation runs fresh and replaces the entry.
      The evaluation is a function of the key and those reads, so a hit returns what a fresh evaluation would.
      Calls with a read trace (the capture's pure-slot trace) or without readers bypass the memo.
    - `APS5_SRT_EVAL_MEMO=verify` also evaluates fresh on every hit and aborts with a stderr message on a difference.
      The 12-loop replay ran clean under it.
    - *Measured.* With the seven switches, a temporary counter (not committed): ~6350 misses, all in the first frame,
      then 60000+ hits, map size 6348. Four interleaved replay pairs (off -> on): capture phase
      8.8 / 10.7 / 9.2 / 8.9 -> 7.6 / 12.8 / 8.7 / 8.1 us per packet (the second pair was noisy throughout); worker
      frame CPU 51.0 / 57.0 / 51.0 / 52.0 -> 48.3 / 53.0 / 50.4 / 52.2 ms. So roughly -0.8 us per packet in capture,
      with the worker total inside the ~10% noise. Less than the 15% inclusive profile share suggested: a hit still
      builds a key (three small vectors) and re-reads every recorded word through the reader, and the replay's
      memory is identical every loop, so this is the best case for hits.
    - *Tests.* `agc_shader_memory_srt_eval_memo_1` and `_verify` run the shader memory tests with the memo on; they
      re-capture a plan whose guest words changed. Negative control: with the value comparison removed from the
      validation, `_1` fails ("cache hit retained stale shader data") and `_verify` aborts with the memo message.
      Full suite with the switch off, `=1` and `=verify`: only the known `agc_driver_vopc_compare` and
      `agc_driver_vopc_compare_table` fail.
    - *Open.* A cheaper validation (the write watch's generations instead of re-reading words). The key's
      allocations and the per-word re-read were tried next (see the second step below).
    - *Second step, tried and reverted.* The key hashed from the call's own spans, with the owned vectors copied only
      on insert (no allocation per lookup). A run reader on `SrtRuntime` (`SrtMemoryRunReader`), implemented by
      `ShaderMemory::readRun` with one page lookup per page and the same results, side effects and early stop as
      `read` word by word (a word on a snapshot region's path goes through `read`). `srtEvalReadsUnchanged` grouped
      consecutive words of one reader at stride 4 into runs, and `=verify` compared each run's results with
      word-by-word reads.
    - *Step 0, where a hit spends its time* (`--sample 15`, 12 loops, the seven switches, `APS5_SRT_EVAL_MEMO=1`,
      486 worker samples). `evaluateMaterializationSources` is 41 samples (8.4% of the worker). Under it:
      `ShaderMemory::read` 18 (44%), the key's hashtable find 14 (34%), allocations 4 (10%), result copies 2, the
      miss path 2, the rest 1. Per hit (replay counters, 3 loops): about 11 recorded reads in 2.5 runs, `userData`
      17.9 words, `sources` 5.1, `cleanFlatSlots` always empty; the first frame has ~6350 misses. The capture's SRT
      evaluate totals 0.30 s over 53581 captures (5.6 us per capture).
    - *What was checked.* The `=verify` replay ran clean (no abort). The full suite passed 305/305 with the switch
      off, `=1` and `=verify`. Negative control (the key matched by plan pointer alone, the hash by plan pointer
      alone): `agc_shader_memory_srt_eval_memo_1` failed and `_verify` aborted with the memo message; restored.
      Sampled with the new code: `evaluateMaterializationSources` 38 samples (8.3% of 458 worker samples), the
      capture's SRT evaluate 0.25 s (4.9 us per capture), one run.
    - *Pairs, new code, switch off -> on* (three interleaved pairs, loops 3-11): worker 55.8 / 51.2 / 49.4 -> 49.2 /
      48.1 / 46.9 us per packet; frame 116.8 / 107.7 / 105.3 -> 105.3 / 101.6 / 101.3 ms; capture 13.5 / 12.3 / 12.8
      -> 10.8 / 10.6 / 10.3 us per packet.
    - *Pairs, committed memo against the new code* (six interleaved pairs, same replay command, loops 3-11,
      binaries built from `b29d8b80` and from the new code): worker 50.0 -> 49.4 us per packet (-1%), frame 107.8 ->
      106.7 ms (-1%), capture 10.8 -> 10.6 us (-2%), SRT evaluate per capture 4.75 -> 4.9 us (+3%). Every difference
      is inside the run-to-run spread (about +-3 us per packet, +-10 ms per frame). Not measurably cheaper, so the
      code was reverted and this note is the only commit.
    - *Why a hit stays at this cost.* With the new code, 12 of the 38 evaluate samples are in `ShaderMemory::page`
      on the first touch of a page in each capture (the pending-write query, the accessibility check, the flush);
      those are side effects the replay must keep. The key and the result copies are small. Removing the per-word
      overhead of the re-read (one call per run, one page lookup per page) left the total unchanged. A compiled walk
      (not started, see below) is the other option.
    - *Measured for its design* (temporary counter in `SrtWalker::EvaluateRuntimeSources`, not committed). With the
      seven switches over 14 loops, the worker evaluates ~4.8 SRT plans per draw (~6400 per frame). Keyed by
      (plan, sources, user data, shader base), there are 6396 distinct inputs, all in the first frame: every later
      call repeats an earlier key. But only 37 of 80000 calls repeat the call just before.
    - So a memo needs a map keyed by those inputs, not a last-call cache. It also needs a validity check on the
      memory the walk read (its `ReadConst` / `LoadAddress` ranges against the write watch), since the replay
      restores identical memory every loop and so overstates the hits.
    - The evaluation is 15% of the worker inclusive. A memo hit would also skip the SRT reads' `GuestMemory::Read`
      calls. At the start of the night the worker waited ~12 us per packet for the committer, so its
    SRT evaluation (11% of the worker) was off the critical path. With all of tonight's switches the two threads are
    nearly balanced: committer ~40.5, worker ~43 us, the queue half full on average ("enqueued behind 2.2"). The
    worker's remaining phases are capture 9.1 us (SRT evaluation ~5), key/lookup 6.5 (`drawRegisterKey` ~2),
    decode 4.6 and recompile 3.0. So the next committer gain needs worker work too: B4a, then the register hash. The
    evaluator already runs on a compact plan with memoized values (`BasicEvaluator<CompactNode>`). A flat compiled
    walk must reproduce its phi selection, raw reads and lane masks exactly; build it with a verify mode that runs
    both and compares.
  - **B5** (dispatch commit items) is built, behind `APS5_PIPELINE_DISPATCH=1` (with `APS5_PIPELINED_DRAWS=1`): `c2e1361e`, on the harness `ee358160`.
    - *What it does.* A direct `DISPATCH_DIRECT` on queue 0 whose dispatch-cache entry has a recipe is a commit item: `PrepareRecipe`, the labels and `DispatchRecipe` run on the committer, in order. Before the lookup it drains only if its entry is missing, or the entry's regions (the CPU-validated reads) overlap a pending write. After the lookup it drains unless the recipe's writes can be ranged: buffers as `visitWrittenBuffers` gives them, and storage images the shader stores to as `DrawWriteRanges` bounds them (surface and DCC key). Buffer device address pointers (`bdaAbiVersion`), GDS, the BDA table bindings and an image descriptor that is not known drain. A recipe rebuilt on the committer falls back to the full dispatch there, and an error in the closure is reported like the other commit items'. Counters: `[dispatch-item]` under `APS5_PROFILE_DRAW`, with the drains by reason (miss, reads, no recipe, writes, verify).
    - *Measured.* Replay, two interleaved pairs, 12 loops, `APS5_NULL_SUBMIT`, the seven switches. DISPATCH_DIRECT (0x15) packet drains per 10 s window, final code: switch off 40 / 203 / 234 and 60 / 248 / 229; switch on 47 / 0 / 0 in both pairs (the first window holds the misses, the rest are commit items). Frame ms (mean of loops 3-11): 105.1 / 117.3 off, 104.0 / 111.3 on. Inside the ~10% noise, so the frame time gain is not shown here. An earlier pair, run before storage images were ranged, drained 153 / 154 per window with the switch on (off 195 / 232 and 188 / 229): the write bound then drained about three quarters of the dispatches (`writes`), all of them storage-image stores (checked with a temporary print, not committed).
    - *Tests.* `agc_driver_pipelined_dispatch` (the switch off) and `agc_driver_pipelined_dispatch_item` (the switch on): each round's output is checked after the wait (the dispatch sees the fill before it), and a second part, whose fill is of a buffer the dispatch does not read, must not drain with the switch on. A third part reads its input with a raw scalar load (`s_load_dword` from the input's address): the capture reads it on the CPU, so its entry covers the input (one 4-byte region there; a `s_buffer_load_dword` through a V# gave no region at all, so that form would not exercise the rule), and with the switch on each round drains once. Full suite, final code: 305 of 305 with the switch off and with `APS5_PIPELINE_DISPATCH=1` (`ctest -j4`, DISPLAY=:99).
    - *Negative control: the read-side rule is verified.* With the overlap check forced off (`readsClear = true`, temporary, reverted), `agc_driver_pipelined_dispatch_item` fails in the third part: scalar read round 1 outputs 324478057 (0x13572469, round 0's fill plus one) where 0x2468ace1 is expected, a stale input. With the rule as committed, all five rounds pass. The first two parts still do not exercise the rule: their entries have no CPU-read region over the input (checked with the same temporary print).
    - *Open.* The replay runs no GPU work, so the pipelined dispatches' output is checked only by the harness. The day machine needs `APS5_PIPELINE_DISPATCH=1` on the live GTA V Performance capture, with frame time and the `[dispatch-item]` line (item 1 of the day list).
  - (The B7 above is the rect-list memo, an item the replay found: the queue's 7.)
  - **B4b.** Measured; it moves cost instead of removing it (below).
- **Blocked.**
  - Nothing measures GPU idle here: B2's effect on the GPU needs the day machine.
  - The container's ~10% noise hides changes below that.

### For the day session (RTX 5080), in order

1. **`APS5_RETIRE_ON_WORKER=1` first** (the night's largest measured change, B8 below; with `APS5_BATCH_SUBMITS=1`
   `APS5_PIPELINE_HYSTERESIS=1`, `APS5_TARGET_KEY_PROOF=1` and `APS5_COLLECT_GRANULE_KIB=64` the best set here,
   `APS5_DRAW_KEY_MEMO=1` a small extra on the worker, `APS5_COMMITTER_SPIN_US=50` the frame-time one, and
   `APS5_PIPELINE_DISPATCH=1` (with `APS5_PIPELINED_DRAWS=1`; check the `[dispatch-item]` line) the dispatch commit items; try
   them added one at a time; for the granule, compare the `[guestmem]` line's `collect N calls` too). The perf replay, `--loop 20`,
   `APS5_PROFILE_DRAW=1`, two runs with and two without; report both threads' us per draw and their frame CPU.
   Expected: both ~10-20% lower. Then live GTA V Performance with and without it: frame time and the `[draw]
   pipelined` line's us per commit. Then `ctest -R agc_driver` with it on Windows.
2. The perf replay, `--loop 20`, `APS5_PROFILE_DRAW=1`, three runs:
   - (a) no switch;
   - (b) `APS5_BATCH_SUBMITS=1`;
   - (c) `APS5_BATCH_SUBMITS=1 APS5_LABEL_FLUSH_US=2000`.

   Per run, report `recorded batches submitted (per frame)`, the `[recorder] submits` / `submit thread` lines, the
   committer's us per committed draw, and frame time. Expected: batches ~93 -> ~65-70 for (b), lower again for (c);
   the committer's per-draw CPU down by the submit cost.
3. Live GTA V Performance, with and without `APS5_BATCH_SUBMITS=1`: the GPU idle per frame (`[gpu]` timing). Growing
   idle means batches start too late; then try `APS5_LABEL_FLUSH_US=500`.
4. Replay with `APS5_COUNT_ALLOCS=1`: the `heap:` line should read ~40 allocations per committed draw (70 before
   `ab5f4e56`). Compare the committer's us per committed draw with the previous build's, two runs each.
5. Run `ctest -R agc_driver` with `APS5_BATCH_SUBMITS=1` on Windows; all must pass, as they do here.
6. Calibrate the instrument: the replay with and without `APS5_NULL_SUBMIT=1` on the RTX 5080. The difference in
   committer us per draw is the GPU submission's cost, which the container cannot see.

### B0: the replay as a CPU instrument

`APS5_NULL_SUBMIT=1` (`feat(agc): APS5_NULL_SUBMIT, the frame replay as a CPU instrument on Linux`) runs the
driver's whole CPU path without GPU work. Everything up to the submit runs (decode, capture, recompile, template
lookups, descriptor writes, command recording), then:

- **Submits.** Every `vkQueueSubmit` keeps its semaphores and fence but no command buffers. The batch completes at
  once, and every wait and retire path runs as after a GPU that finished.
- **Pipelines.** Guest pipelines (graphics and compute) are one stub pipeline, an empty compute shader, made once
  per device. They were stubbed rather than created lazily, because lavapipe cannot compile what the recompiler
  emits for NVIDIA. Pipeline *lookups* still run: only the one-time `vkCreate*Pipelines` is gone.
- **Device as NVIDIA.** The device reports what the day machine has: 32-wide subgroups in every stage (lavapipe has
  8 lanes and no vertex-stage subgroups, which made the recompiler emit different code), sample counts 1/2/4/8,
  tessellation (rect lists) and depth bounds.
- **Where lavapipe can't follow.** `vkCreateImage` makes 2 and 8 samples at 4. A host import over lavapipe's
  2 GiB imports its first GiB; the addresses past it only go into tables and recorded commands, never dereferenced.
  The texture detiler reuses one descriptor set, because lavapipe gives every set a mapping of its own.

What GPU work would have written is not written: read-backs and write-backs see what memory holds.

Container fixes the replay needed. Each one made the numbers more like the day machine's; together they took the
committer from ~14 s to ~0.1 s per frame:

1. **`vm.max_map_count`.** Lavapipe maps every descriptor set, and the replay crashed in `vkAllocateDescriptorSets`
   at the default 65530 mappings. `replay-bench.sh` raises the limit.
2. **Page-state cache on Linux (no switch, Linux only).** `GuestMemory`'s Linux checks reread `/proc/self/maps`
   for every call: Windows caches `VirtualQuery`'s answers, Linux cached nothing. They now cache the same way, over
   the image range and a new `SetGuestPagesRange` where no guest arena exists, which the replay names.
   `APS5_NO_LINUX_PAGE_CACHE=1` restores the old behaviour. The replay forgets the cache after its own protection
   changes.
3. **Write watch.** The replay registers its guest memory with the Linux write watch, as `DirectMemory` does in the
   emulator. Without it every surface was "untracked", so every use re-uploaded it and wrote it back
   (~100 GB per 10 s).

Replay output: per loop, `draws P packets, C committed (N per frame); worker X us per packet, committer Y us per
committed draw`, from new counters (`DriverDrawPackets`, `DriverDrawsCommitted`) and last night's thread clocks.

`tools/perf/replay-bench.sh [--loops N] [--label L] [-- ENV=VALUE ...]` builds, runs, and prints one line
(`tools/perf/replay_summary.py` parses the log).

Baseline (this container, 2026-10-08, 47 loops after 3 warm-up loops, `replay24.log`):

| | container | live (day machine) | ratio |
|---|---|---|---|
| draws per frame | 1343 | ~1780 | the replay's 3 frames include partial ones |
| committer, us per committed draw (thread CPU / draws) | 84 (+-5 between loops) | ~22.7 | 3.7 |
| committer phases inside `Graphics::Draw`, us per draw | 64 | 22.7 | 2.8 |
| - record | 20.1 (bindings 15.5) | 8.6 (bindings ~3.0) | 2.3 (bindings 5.2) |
| - lookup | 12.6 | 5.4 | 2.3 |
| - vertex | 13.5 | 4.2 | 3.2 |
| - readTarget | 5.5 | 2.3 | 2.4 |
| - pipeline | 3.2 | 1.1 | 2.9 |
| - validate + resources + keep + setup + prepare | 9.0 | ~3 | 3 |
| worker, us per draw packet | 92 (decode 9.5, capture 22.2, recompile 5.9, key/lookup 16, rect-list 19, lock wait 23) | capture 6.9, recompile 5.6, decode 4.3, key/lookup 4.6 | 2-3.5 |

How far to trust it:

- **Proportions.** The committer's phases are 2.3-3.2x the live ones, uniformly, except `record.bindings`
  (5x: lavapipe's `vkAllocateDescriptorSets`/`vkUpdateDescriptorSets` cost more than NVIDIA's). The container's CPU
  is slower than the Ryzen 5 7600 (4 shared vCPUs).
- **Absolute numbers.** Use ratios between runs on the same machine, never the absolute microseconds.
- **Noise.** Loop-to-loop spread is ~6%, run-to-run ~10% (84 vs 93 us). An A/B needs at least two runs each, and
  a difference under ~10% is noise.
- **Outside the draw phases.** The committer's CPU per draw (84) exceeds its draw phases (64): labels, DMA and
  dispatch commit items, drains and submits run on it too.
- **The worker.** Its `lock wait` (23 us) is time spent waiting for room in the pipeline, which is the committer's
  cost.

Day-session check: none needed for B0 itself (a tool). To calibrate: run the same replay on the RTX 5080 with
`APS5_NULL_SUBMIT=1` and compare its `committer us per committed draw` with the normal run's. The difference is the
GPU submission's CPU cost, which this mode leaves out.


### B1: splitting the committer (state analysis and design; not implemented)

What `Graphics::Draw` (`Graphics/src/Draw.cpp`) reads and writes in each phase, under the GPU mutex. Container
microseconds are from the baseline above; live ones from the queue.

| Phase (us here / live) | Reads | Writes | Off the committer? |
|---|---|---|---|
| validate (2.3 / -) | the compiled shaders, `CachedFragmentOutputs` memo (own mutex), device limits | the memo | **yes**: pure |
| vertex (13.5 / 4.2): `prepareDrawInputs` | guest memory of index and vertex buffers (`CheckRange`, `CopyDrawInput`), the recorder's snapshot reuse map, the snapshot ring, the draw-input memo (thread-local) | snapshot buffers, the reuse map (`KeepDrawInput`) | **with a condition**: the copied ranges must hold their final bytes, i.e. no earlier draw or dispatch in the window and no pending GPU result writes them. `DrawPipeline::Queue0().Overlaps` answers the first, the pending registries (`PendingWrite`, `StorageTexture::PendingSerial`) the second; otherwise fall back to the committer. The reuse map and the ring need a lock or per-thread arenas. |
| readTarget (5.5 / 2.3): `refreshResidentTarget` | the storage-image cache, the target's pages (`CollectWrites`, write-tracker stamps), DCC keys, pending results | the image's sync state (upload, write-back) | **no**: an earlier draw in the window renders the same targets, and the refresh decides uploads and write-backs against them. It stays on the emit side. The memo (`APS5_TARGET_PROOF_MEMO`, on) already makes repeats cheap. |
| lookup (12.6 / 5.4): key 2.2, find 1.0, revalidate 5.5, moved 3.0 | `SharedResourceCache` (own mutex), the texture caches, the pending-image registry, guest-memory generations | the cache's LRU, invalidations | **key and find: yes** (pure, plus the cache's own mutex). **Revalidate and moved: only as a guess** that emit re-checks: they prove the template against texture identity and memory state that earlier draws in the window may change. A prepared proof would carry the `PendingSerial` and collect epoch it saw, and emit redoes it if either moved, which is the fast path the lookup memo (B3 last night) already builds. |
| resources (2.2 / -) on a template miss: `ShaderResources` build (~550 us) | everything the lookup reads, plus uploads, BDA leases | new descriptor sets, buffers | **mostly**: a build on a prepare thread is the biggest per-miss win (12% of live draws miss, ~8 ms per frame). It allocates from the descriptor cache (needs a lock) and may take a registry lease (address-based builds must stay on the committer). |
| pipeline (3.2 / 1.1) | `PipelineStore` (own mutex; identity fast path on) | the store | **yes** |
| record (20.1 / 8.6): bindings 15.5 (snapshot fills, descriptor set allocation and writes), begin 1.7, commands, push | the recorder's open batch, the render-pass continuation key, queued key and label stores | the command buffer, the batch's keep lists | **bindings: yes** (snapshot fills and `vkUpdateDescriptorSets` need no command buffer; the descriptor allocator needs a lock or per-thread pools). **The rest: no** (`vkCmd*` into the one command buffer, in order). |
| keep (3.2) | the recorder | the batch's lists | no |

Design (`APS5_SPLIT_COMMIT=<threads>`, not started tonight):

1. **Prepare.** The committer's queue already holds each draw's commit closure (`DrawPipeline`). Prepare is a
   second closure enqueued with it: validate, the pipeline lookup, the resource key and find, a template build on a
   miss (not address-based), the vertex and index copies when `Overlaps` and the pending registries clear them,
   and the binding fill. Prepare threads take draws in sequence order, up to N ahead of emit.
2. **Emit.** On the committer, in order: refresh the targets, re-prove the template (the memo fast path when the
   proof's serial and epoch still hold, otherwise the full revalidate), redo any vertex or index copy whose range
   became dirty, then record.
3. **Ordering.** A prepare result names the pending-item sequence number it saw (`a75aea29`'s
   `APS5_PARALLEL_DRAWS` numbering). Emit compares it with the serials of the commits between, and falls back to
   the inline path when one of them wrote a range the prepare read.

Why not tonight: the state above has five owners (recorder, resource cache, texture caches, pending registries,
write tracker), each with its own lock regime. Getting the fallback conditions right needs the replay **and** a GPU
to check pixels; a wrong one renders stale vertices. The queue's other measurable items came first.

Cheaper first step with the same shape: prepare on the **worker** instead of new threads. The worker waits ~25% of
its time for room in the pipeline (`lock wait` 23 us per packet here), so moving validate, the pipeline lookup and
the resource key and find (~10 us here, ~4 live) onto it would balance the two threads without new
synchronization: these are pure or behind their own mutexes.

### B2: fewer submits, and a submit thread (`APS5_BATCH_SUBMITS=1`)

Instruments (no switch needed to read them):

- The replay prints `N recorded batches submitted (X per frame)` per loop (new counter `DriverBatchesSubmitted`,
  counted in `Recorder::Submit` for every open batch), and `replay-bench.sh` reports `batches/frame`.
- `APS5_TRACE_SUBMITS=1` prints a `[submits]` line every 10 s: the submits by (queue, packet, six callers), with the
  dispatches each batch held. Symbolize the offsets with `addr2line -f -C -e build*/.../libSceAgcDriver.prx`.
- Bug fix: `GuestMemory::CaptureCallerOffsets` captured one frame on Linux (raw address). It now walks the stack
  with `backtrace` and gives offsets into the holding object (`dladdr`), as on Windows. The `[hooksync]` lines get
  their callers on Linux too.

Where the batches end, baseline (one 10 s window of about 12 frames, 1111 batches):

| Site (callers) | Batches per 10 s | Per frame | Dispatches per batch | Guest-observable? |
|---|---|---|---|---|
| `DrawPipeline::run` -> `submitOpenWork`: end of a REWIND segment whose tail the guest already released (`PacketExecution.cpp`), or a submission's completion | 469 + 40 | ~42 | 10-18 | **no** for a released tail: the worker continues into the tail without waiting |
| `commitDraw` -> `submitDueAfterCommit`: a pending label older than `APS5_LABEL_FLUSH_US` (1 ms) or 64 items (`APS5_BATCH_CAP`) | 332 | ~28 | 26-40 (the deadline, not the cap) | a fallback for polls the driver does not see |
| no packet: `SyncThrough` from the memory hook (the replay writing its memory deltas over pending writes) | 224 | ~18 | 0 | yes (a CPU access) |
| `waitMemory` on queue 0x38 (WAIT_REG_MEM) | 23 | ~2 | 4 | yes |
| `commitLabel` with an EOP interrupt, `flushBetweenPackets`, the flip | ~20 | ~2 | | yes |

Change, under `APS5_BATCH_SUBMITS=1`:

1. **Submit thread.** `Recorder::Submit` ends the command buffer and hands `{commands, timeline value, fence}` to
   one submit thread (`Graphics/src/Resources.cpp`, `EnqueueSubmit`), which calls `vkQueueSubmit` in order. The
   batch counts as in flight from `Submit` on: waits on its fence or timeline value before the thread submitted are
   valid Vulkan and only last longer. Every other user of the queue goes through `AcquireQueue()`: present, the clear
   submit, `CommandBatch` (uploads), the import probe, `vkQueueWaitIdle`, `vkDeviceWaitIdle`, and the test's
   `WaitQueue`. `AcquireQueue` waits until the thread has submitted everything enqueued before the call, then holds
   the queue for the caller. A failed submit on the thread is rethrown at the next enqueue or `AcquireQueue`.
2. **No submit at a released segment end.** The `submitOpenWork` that `PacketExecution.cpp` queued when a REWIND
   segment ended with its tail already released is skipped; the batch stays open into the tail. Labels, waits, the
   cap, and the deadline still end it.

Measured (12 loops each, 3 warm-up loops left out; runs interleaved):

| Run | Batches per frame | Committer us per committed draw |
|---|---|---|
| base | 69.9 / 66.9 / 69.5 | 94.6 / 84.4 / 91.8 |
| `APS5_BATCH_SUBMITS=1` | 52.1 / 53.2 | 80.5 / 83.3 |
| `+ APS5_BATCH_CAP=256` | 46.7 | 91.6 |
| `+ APS5_BATCH_CAP=256 APS5_LABEL_FLUSH_US=4000` | 24.0 | 86.9 |

- Batches per flip go down 23% with the switch alone, and 65% with a 4 ms label deadline.
- The committer's CPU change is within this container's noise (~10%), and is not expected to show here: a lavapipe
  submit without command buffers costs ~20 us. In this VM a condition-variable wake costs ~14 us (measured
  standalone), so the enqueue costs ~18 us here, about what the submit cost. On Windows the wake costs a few us, and
  NVIDIA's `vkQueueSubmit` is the part that goes off the committer.
- With the switch, the remaining batches end mostly at the label deadline after draws (~40 per frame here). The
  deadline is the fallback for guest polls the driver cannot see, so the switch leaves it alone; on the day machine
  the committer is ~3x faster, so 1 ms covers ~3x more draws than here.

Tests: all 131 `agc_driver*` tests that run on lavapipe pass with `APS5_BATCH_SUBMITS=1` and without it
(`APS5_BATCH_SUBMITS=1 ctest -R agc_driver`).

Expected on the day machine: batches per flip from ~93 down to ~65-70 with the switch alone. The committer stops
paying `vkQueueSubmit` (~20-60 us on NVIDIA, about 2-5 ms per frame at 93 submits). GPU idle gaps may grow if
batches start later; that is what the day check measures.

Day-session check:
1. Run the replay `--loop 20` with `APS5_PROFILE_DRAW=1`, with and without `APS5_BATCH_SUBMITS=1`. Compare the
   `recorded batches submitted (per frame)` lines, the `[recorder] submits` and `submit thread` lines, the
   committer's us per committed draw, and frame time.
2. Run live: GPU idle per frame (the `[gpu]` timing lines), with and without the switch. Then add
   `APS5_LABEL_FLUSH_US=2000` and `4000`, and keep the smallest deadline that gives no idle growth.
3. Check pixels: a replay with `--hash-check` gives the same hashes with the switch on.

### B8: where the driver threads spend their CPU, and the commit's teardown (`APS5_RETIRE_ON_WORKER=1`)

**Instrument** (`d494501f`, `APS5_CPU_SAMPLE=1`). gdb sampling of the replay (40 samples of every thread) found the
committer 91% and the worker 95% in futex waits, and the tool's main thread busy 34/40 in `Replayer::restore`: the
driver threads run ~3% of the wall time, the rest is the replay restoring guest pages between loops. So wall-clock
sampling cannot profile them. `APS5_CPU_SAMPLE=1` arms a process-wide `ITIMER_PROF` (1 ms of process CPU) from the
second loop on; the SIGPROF handler keeps the stack only when it lands on a registered driver thread
(`DriverThreadClock`). Samples go to `APS5_CPU_SAMPLE_FILE` (default `cpu-samples.txt`) as module offsets;
`tools/perf/cpu-profile.py FILE [--role worker|committer] [--callers F | --callees F | --stacks F]` symbolizes with
`addr2line -a -f -C -i` and sums by function. About 60 samples per role per loop: 20 loops give ~1000 each. Linux
only (Windows has ETW / VTune for the same view). `tools/perf/replay-bench.sh --sample [N]` does both: one run, the
summary line, then each thread's top N functions and the samples file's path.

**Committer, before** (625 samples): 12.5% destroying the commit closure (`~DrawTail`: `DrawEntry`, `DrawDecode` and
`DispatchVariant` releases, `ShaderMemory`'s pages, vectors), 12.8% in write-watch collection (`ioctl`), ~15% in
malloc/free.

The closure owns everything the worker prepared, and `DrawPipeline::run` popped the item **under the pipeline
mutex**, so the worker's next `Enqueue` also waited for those frees. `ShaderMemory` keeps a per-thread page pool:
pages freed on the committer went to the committer's pool, where nothing uses them, and the worker allocated new
pages for every capture. Every free of a worker allocation was a cross-thread free (glibc: the other arena's lock,
then `malloc_consolidate`).

**Change** (`bbc54b47`, `APS5_RETIRE_ON_WORKER=1`). The committer moves the finished commit into a `retired` list
and frees the rest of the item after releasing the mutex. `Enqueue` / `EnqueuePending` (the queue worker) swap the
list out and free it after their own unlock. At most 1024 wait (no `Enqueue` for a while): beyond that the
committer frees. Only the thread and time of the frees change. Recipes refer to variants through `weak_ptr` and
match by identity, so a variant freed a little later changes nothing.

Measured (12 loops, two runs each, interleaved):

| Run | Committer us per draw | Worker us per packet | Worker capture / key-lookup / decode |
|---|---|---|---|
| base | 61.7 / 62.7 | 61.3 / 62.6 | 16.9-17.5 / 13.6-13.9 / 8.0-8.3 |
| `APS5_RETIRE_ON_WORKER=1` | 50.3 / 50.0 | 48.0 / 47.7 | 10.4-10.6 / 7.5-7.6 / 5.2-5.3 |

Combined with the other switches (12 loops, one run each):

| Run | Committer us per draw | Worker us per packet | Batches per frame |
|---|---|---|---|
| `APS5_RETIRE_ON_WORKER=1 APS5_BATCH_SUBMITS=1` | 46.0 | 45.2 | 40.3 |
| `APS5_RETIRE_ON_WORKER=1 APS5_PARALLEL_DRAWS=2` | 58.8 | 38.4 | 66.4 |

Retire plus batched submits is the best set measured here. Adding `APS5_PIPELINE_HYSTERESIS=1` removes most of the
committer's remaining wake calls: when the worker waits for room in the full pipeline, every commit used to wake it
for one slot. Now it is woken at half depth. Sampled: the committer's wakes 4.2% -> 1.2% of its CPU, worker samples
794 -> 738; us per draw moved inside the noise (committer 45.8 / 45.0 -> 45.1 / 44.8). `APS5_TARGET_KEY_PROOF=1`
then removes the per-draw scan of the render target's DCC keys (`materializeRegisterClear` -> `CurrentDccKeys`,
132 KiB for a 4K target). It keeps a proof per key range, as `ProvedClearKeys` does for textures: `AllKeysEqual`
(3.3% of the committer) leaves its profile. With parallel draws the worker gets faster but the
committer slower (its commits wait on the helpers' published items). The full suite passes with retire plus batched
submits (all but the two `vopc_compare` tests, as without them).

The worker got faster although it now does the frees: its allocations are reused from its own caches and pool.
Tests: the full ctest suite (300) with the switch on: all pass except `vopc_compare` / `vopc_compare_table` (fail
without it too, as in B6).

Expected on the day machine: the Windows heap's cross-thread frees are cheaper than glibc's arena hand-off, so
probably less than here. The committer's share should still be several us per draw (the closure's teardown is the
same work), on the thread that limits the frame.

**Committer, after** (858 samples): `Graphics::Draw` 80%; `recordDraw` 27% (much of it lavapipe's own command
recording, `vk_icdGetInstanceProcAddr`, not representative); `resolveDrawResources` 21%; `PrepareDrawBindings` 18%;
`prepareDrawInputs` 17%. Write-watch collection is 14%: `PrepareDrawBindings` 6.3%, `CopyDrawInput` 2.8%, label
writes (`commitLabel` -> `GuestMemory::Write` -> `storeOwn`, which walks twice) 2.8%, `fastRevalidate` 1.4%. The
collect memo hits ~1/3 (`collect` ~105k calls per 10 s, ~0.9 MB each). On Linux each collect is an `ioctl`; on
Windows it is `GetWriteWatch`, so how much of this transfers is unknown (the day session's ETW profile decides).
`steady_clock::now` is 5.7% on the committer and 7.9% on the worker: that is `APS5_PROFILE_DRAW`'s own timers,
which every measurement with it carries.

**Worker, after** (814 samples): `drawTail` 64%; `compileDrawStage` 28%, of which the capture is 16% (the SRT
evaluation, `EvaluateRuntimeSources`, 11%: B4a's target) and recompile 8%; `DecodeVertexStageInfo` 7%;
`cacheDrawStages` 5.5%.

**Dispatch drains (B5), measured, not implemented.** Per 10 s of the replay (~19 frames): 357 packet drains, 75 ms of
worker waiting, of them `DISPATCH_DIRECT` (0x15) 233, `DISPATCH_INDIRECT` 49, `EVENT_WRITE` 30, 0x10 and 0x3c 15
each. A direct dispatch's phases after its drain: dispatch-cache validate 2.4 us, recipe pre-check 12 us, device call
38 us (all 457 dispatches per 10 s were cache hits). So ~12 dispatches per frame each leave the committer idle for the
dispatch plus the refill: roughly 2-3 ms per frame here. The cheapest correct cut of last night's design: only
dispatch-cache recipe hits become commit items, with `PrepareRecipe` / `DispatchRecipe` (and their revalidation)
moved into the commit closure, in order on the committer; the worker keeps decode, capture and the cache lookup,
draining first when a validated run overlaps `DrawPipeline::Queue0().Overlaps`. Not implemented tonight: nothing in
the container can check its ordering. The replay does not execute GPU work, and no test drives draws and dispatches
through PM4 with real shaders. The test the queue asks for (a dispatch reading what a preceding draw wrote) needs that
harness first.

**Committer allocations and write-watch walks, after B8.** About 15% of the committer is still malloc/free. Most of it
is glibc's `malloc_consolidate`, set off by a few large per-draw blocks (`CopyDrawInput`, `AcquireFramebuffer`,
`NotePendingReads`). The Windows heap does not work that way, so it was not chased here. The write-watch walks (14%):
the per-thread memo ring and the per-epoch map already catch repeats within an epoch (~100 draws). The ~4 walks per
draw that remain are ranges new to the epoch (per-draw constant data). Fewer of them needs fewer epoch bumps (waits)
or a cheaper walk, not a better memo.

**Worker, with retire + batching + hysteresis + key proof** (797 samples): `futex_wake` 7.2%, almost all from
`DrawPipeline::push` waking the committer that found the queue empty (5.3% for draws, 1.4% for labels). This is
the same ping-pong as the room wait, the other way round. Not changed: deferring that wake until a second item is
queued would need a wake on every path where the worker goes idle (end of submission, waits, drains), or a draw could
sit unrun. A short spin in the committer before it sleeps is the safer form to try. `drawRegisterKey` is 4.5% (the
register hash of the draw cache key), the SRT evaluation ~5% (B4a).

Also measured tonight (two runs each, no change kept): `APS5_DRAW_INPUT_MEMO=1` and `APS5_LOOKUP_MEMO=1` (last
night's B3 / B4) move neither thread's us per draw outside the noise in the replay. Vectorizing the index scan in
`Draw.cpp` had no measurable effect either and was reverted.

Day-session check: item 1 of the list above.

### B7: rect-list shaders built per draw (the replay's biggest worker phase)

The replay's worker spent ~17 us per draw packet, averaged over all packets, in `rect-list`. That is
`ShaderRecompiler::BuildRectListShaders`, called for every rect-list draw. It emits the tessellation control and
evaluation modules that turn a rectangle into two triangles, then runs SPIR-V Tools validation and optimization on
both; most of the worker's `spvtools::val` allocations (B6) came from here.

The modules depend only on the fragment parameters (after the device's capacity cut), the fault binding and the
SPIR-V and Vulkan versions. They are now kept by that key (at most 256 entries, mutex-protected) and copied out. No
switch, since the modules are identical; `APS5_NO_RECT_LIST_MEMO=1` builds every time.

Measured (two runs each, interleaved, 10 loops):

| Run | Worker us per packet | Worker frame CPU | Committer us per draw |
|---|---|---|---|
| memo | 59.4 / 62.5 | 80 / 84 ms | 59.8 / 64.0 |
| `APS5_NO_RECT_LIST_MEMO=1` | 79.7 / 77.9 | 107 / 105 ms | 63.2 / 61.0 |

Tests: `agc_driver_graphics` (rect lists) and the draw tests pass.

Expected on the day machine: the live phase table has no rect-list row; at the container's ~2.5x ratio it would be
~7 us per packet, about 12 ms of worker CPU per frame at 1780 draws. Worker time is off the critical path while it
waits for the committer (`lock wait`), so frame time moves only where the worker was the limit.

Day-session check: the `[draw] driver phases` line should no longer show `rect-list`, and the worker's us per packet
should drop.

### B6: committer heap traffic

Instrument: `APS5_COUNT_ALLOCS=1` (`tools/FrameReplay.cpp`). The replay replaces the global `operator new`, which the
driver's library binds to as well, and counts allocations and bytes per driver thread role. The role comes from a
new `CurrentDriverThreadRole()`, set by `RegisterDriverThread`. One committer allocation in 31 also records its five
callers. Per loop it prints `heap: committer N allocations (B bytes) per committed draw, worker ... per draw packet;
committer sites (sampled): [...]`, with module offsets (`addr2line -f -C -i -e .../libSceAgcDriver.prx`). Without
the switch the replacement only forwards to `malloc`.

Measured: committer **70.7 allocations (8.8 KB) per committed draw**, worker 230 (28 KB) per draw packet. The top
committer sites, per draw:

| Site | Allocations per draw | Change |
|---|---|---|
| `BuildVertexInputLayout` | ~13 | `std::set` of locations replaced by a bit set; the two vectors reserved; the "unsupported vertex format" message built only on failure (it was built for every attribute, as `Require`'s argument) |
| `AnyShadowedOverlaps` (from `MovedReadOnlyBuffers` and `Revalidate`) | ~6 | walks the shadow registry in place instead of copying the overlapping `shared_ptr`s into a vector |
| `GuestBufferMemory::InPlaceReads` (from `PrepareDrawBindings`, `MarkGpuWrites`, `ReadsOverlap`) | ~6 | an overload that fills a caller's vector (thread-local scratch in both per-draw callers); `ReadsOverlap` asks `InPlaceReadOverlaps` without building the list |
| `MovedReadOnlyBuffers` | ~3 | `MovedBuffer::words` is a view of the compiled program's descriptor words, not a copy (the list lives only for the draw) |
| `DrawResourceKey` / `ShaderResources::ContentKey` | ~2 | `AppendContentKey` writes each stage's words into the key in place |
| `PrepareDrawBindings` | ~3.5 | the snapshot list reserved |

No switch: same results, fewer allocations. After: **40.7 allocations (6.8 KB) per committed draw (-42%)**. The
committer was 76.4 then 69.0 us per committed draw in these two single runs; that is inside the ~10% noise, so it is
not claimed as a gain. The rest is spread over many sites at about one allocation per draw each: the vertex layout's
two vectors (kept in the draw inputs), the bindings object, the resource key itself, small vectors inside
`Graphics::Draw`.

Tests: the `agc_driver*` suite passes except `vopc_compare` and `vopc_compare_table`. Both fail the same way at the
branch head without these changes since the container restarted mid-night ("thread 1 compare 39 is 0"): lavapipe's,
as the hw-rt progress file records for those two.

Expected on the day machine: allocator time on the committer (the Windows heap, ~50-100 ns per allocation) falls by
~30 allocations per draw, about 2-4 ms per frame at 1780 draws.

The worker (`APS5_COUNT_ALLOCS=worker` samples its sites instead) makes ~225 allocations (28 KB) per draw packet:
SPIR-V validation inside recompiles (`spvtools::val`), `Driver::cacheDrawStages` (~14 per draw), `drawTail` (~5, the
largest by bytes), `ShaderMemory`'s constructor (~4), `DecodeVertexStageInfo` (~6), and hash-map nodes (~8). Not
changed tonight: the worker spends ~25% of its time waiting for the committer (`lock wait`), so its allocations are
off the critical path until B1 moves work onto it.

Day-session check: replay `--loop 20` with `APS5_COUNT_ALLOCS=1 APS5_PROFILE_DRAW=1`; the `heap:` line should read
~40 allocations per committed draw. Compare the committer's us per committed draw with the previous build's, two runs
each.

### B4b: the Execution draw cache, measured (no change)

`APS5_NO_DRAW_SRT_ENTRIES=1` A/B on the replay, two runs each:

- The worker's `key/lookup` phase drops ~17 us per packet.
- `decode` grows ~8 us and `vectors` ~4 us: the cached entry also lets the worker skip decoding
  (`decode skipped 19722` per 10 s in the base run).
- Net: ~5 us per packet, under the noise floor.

Dropping the cache only moves the cost, so there is no change. A re-key would have to keep the decode skip; that is
B4a's compiled SRT walk, not started.

## Night 2026-10-07

Base: `a75aea29` (`gta-v/fidelity-main` + the pipelined-draws work). Built in `build-perf` on lavapipe; the
`agc_*` ctest suite (143 tests) passes with the switches below off and on, except `agc_driver_vopc_compare` and
`agc_driver_vopc_compare_table`, which fail at the base commit too. They are lavapipe's: `gta-v/hw-rt`'s
`PROGRESS.md` (section D) has the evidence. The suite also passes with all the B switches on together, plus
`APS5_PIPELINED_DRAWS=1` and `APS5_VERIFY_PROOFS=1`.

Build fix carried over from `gta-v/hw-rt`: `ShaderDiskCache.cpp`'s Linux layout asserts match the current structs
(`fix(shader): the disk cache's Linux layout checks match the current structs`). The local `HWRT_LOCAL_BUILD`
workaround is gone, so `night/perf` builds on Linux as checked out.

### Commits

| Task | Commit | Switch | What the test proves |
|---|---|---|---|
| B7 exit crash | see `git log` (`fix(agc): retire the buffer pool ...`) | none (bug fix) | `agc_driver_buffer_pool_retire`: a retired pool frees its retained slots while the device lives and makes no Vulkan call for anything returned later, including a `Buffer` destroyed after the teardown (the image-mirror case) |
| B10 recompile copy | `perf(agc): share the draw stage's recompile result ...` | none (no behaviour change) | the `agc_*` draw tests pass with `APS5_PIPELINED_DRAWS` off and on; the CPU-indirect path keeps its own copies, now for every stage |
| B10 variant scan | `perf(shader): most recent variant first in the recompile's variant scan` | `APS5_VARIANT_MRU=1` | the `agc_*` suite passes with it on and off (baseline vopc failures aside); the scan finds the same variant in any order, since at most one matches. Measurement only otherwise: `[recompile] variant scan` under `APS5_PROFILE_DRAW` |
| B5 pipeline identity | `perf(agc): identity fast path for repeated pipeline lookups` | `APS5_PIPELINE_IDENTITY=1` | the `agc_*` suite passes with it on (alone and with `APS5_PIPELINED_DRAWS=1`) |
| B8 replay on Linux | `Frame replay: host memory and process calls behind a small platform layer` (cherry-picked from `gta-v/hw-rt` `d9a73a6f`) | none (tool) | `agc_frame_replay` builds on Linux here; `--summary` reads a capture |
| B8b driver CPU per frame | `feat(agc): per-frame CPU time of the queue 0 worker and the committer in the replay` | none (measurement only) | `agc_driver_driver_thread_clock`: a registered thread's clock, read from another thread, counts its own CPU (at least 180 ms of a spin the thread itself measured at 200 ms of CPU) and not the reader's sleep. The spin was first 200 ms of wall time, which a loaded machine (ctest -j8) can run for less than the 100 ms the test asked; it now ends on the thread's own CPU clock and passes beside eight busy processes |
| B1 snapshot ring | `perf(agc): draw snapshots as slices of shared arenas` | `APS5_SNAPSHOT_RING=1` (`APS5_SNAPSHOT_RING_KIB`, default 1024) | `agc_driver_recorder_tests` (not a ctest on this branch; run by hand) `snapshotRingTests`: slices aligned for a storage descriptor, non-overlapping, one arena until full, a new one after, over a quarter arena refused, an arena released with its last slice. The reuse and in-flight tests there now check slices by (buffer, offset) and copy from the slice's offset; they pass with the switch on and off. The `agc_*` suite passes with it on (also with pipelined draws). |
| B2 target proof memo | `perf(agc): memoize a storage image's refresh proof within a collect epoch` | `APS5_TARGET_PROOF_MEMO=1` | `agc_driver_recorder_tests` `targetProofTests` (runs with the switch set): a repeat within the epoch is answered by the proof; a stamp over the surface, a new epoch after a CPU store, and another image marked dirty over it each force the full Refresh (which then uploads for the CPU store). The `agc_*` suite passes with it on (also with pipelined draws). |
| B3 lookup memo | `perf(agc): re-prove a reused template from its last fast proof` | `APS5_LOOKUP_MEMO=1` | `agc_driver_lookup_memo` (sets the switch and `APS5_VERIFY_PROOFS=1`, which runs the fast proof beside every memo hit and aborts on a disagreement): a repeat within the epoch is answered by the memo; a pending-registry change, a new collect epoch and a registry mutation each force the fast proof; results never change. The `agc_*` suite passes with it on. |
| B4 draw input memo | `perf(agc): per-thread memo of reused draw inputs within a collect epoch` | `APS5_DRAW_INPUT_MEMO=1` | `agc_driver_recorder_tests` `drawInputMemoTests` (with the switch set): a repeat within the epoch is answered by the memo (same buffer, same derived value); a stamp, a new epoch after a CPU store, a pending-registry change and a recorded pending write each force the full path. The `agc_*` suite passes with it on, and with all B switches on together. |
| B6 pipelined-draw prerequisites | `fix(agc): storage images and exact target ranges in the pipelined draws' write ranges` | storage images: none (a fix to the pipelined path, which is itself off by default); exact ranges: `APS5_EXACT_DRAW_WRITES=1` | `agc_driver_draw_write_ranges`: a storage image a draw stores to is a write range (surface and keys), one it only reads is not, one without flags counts as written; the exact color/DCC/depth/stencil/HTILE ranges hold every addressed texel and nothing past the layout; the estimates cover the exact ranges; 4-sample targets scale. |
| B8c flip numbering | `feat(agc): record each present's flip serial; replay by it under a switch` | `APS5_REPLAY_FLIP_SERIALS=1` (replay); the capture always records the serial in the event's reserved bytes | not tested end to end (no game capture here; the flip test's fake output never calls `Driver::Present`). `--summary` now lists every present's Present-count index beside its flip serial |
| B8a replay main thread | `perf(agc): replay restores its page map from an undo log` | none (tool; same results) | builds on Linux; not run end to end (no game capture here) |
| B9 DMA_DATA commit items | `perf(agc): pipelined DMA_DATA as ordered commit items` | `APS5_PIPELINE_DMA=1` (with `APS5_PIPELINED_DRAWS=1`) | `agc_driver_pipelined_dma`: an immediate fill and a copy of it in one DCB land as the CPU path stores them, in order (the copy sees the fill), both committed, none drained. The `agc_*` suite passes with it on, with and without pipelined draws. Dispatch commit items: design only (below) |

### Blocked, and why

- **B8d** (frame 1 differs between identical runs): needs a game capture to compare runs; none is in this container.
- **B8a/B8c, end to end:** the changes build and run on Linux, but without a game capture they could not be measured
  or checked against live frames.
- **Dispatch commit items** (B9, second half): design only, because the stage A/B split for dispatches with images
  needs an argument this night could not test (see B9).
- **B10:** the `RecompileResult` copy is gone and the variant scan can run most recent first (`APS5_VARIANT_MRU=1`).
  The snapshot hash is unchanged: the result memo trusts it without a full compare, so a cheaper hash would add
  collision risk. Both are now measured under `APS5_PROFILE_DRAW` (`[recompile] variant scan`).
- **CMASK:** keeps an estimated range in B6 (no exact formula in the code).
- **Test infrastructure, fixed later in the night:** `agc_driver_recorder_tests` used to stop at `unitShadowTests`
  (and `storageRefreshTests`). Both assume a GPU store into a host import is invisible to the write watch, which does
  not hold on lavapipe, where the GPU is CPU threads. They now detect this and skip (`GpuStoresLookLikeCpuStores`,
  cherry-picked from `gta-v/hw-rt`), and the binary is the ctest `agc_driver_recorder`. It passes, including with
  `APS5_SNAPSHOT_RING`, `APS5_TARGET_PROOF_MEMO` and `APS5_DRAW_INPUT_MEMO` on.

### For the day session

1. Build `night/perf`. Run GTA V to the Lombank prologue with `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` and save
   the 30 s profile as the baseline.
2. Add each switch on its own, same spot, 30 s each. Compare the committer's columns:
   - `APS5_SNAPSHOT_RING=1`: record, and the `[draw-bindings]` create/fill parts;
   - `APS5_TARGET_PROOF_MEMO=1`: readTarget, and the `[target-proof]` counts;
   - `APS5_LOOKUP_MEMO=1`: lookup revalidate, and the `[rescache] revalidate` memo counts;
   - `APS5_DRAW_INPUT_MEMO=1`: vertex;
   - `APS5_PIPELINE_IDENTITY=1`: pipeline, and the repeats on `[pipecache]`;
   - `APS5_PIPELINE_DMA=1`: the `packet 0x50` drains;
   - `APS5_EXACT_DRAW_WRITES=1`: the drain counts.
   - `APS5_VARIANT_MRU=1` (on the worker): recompile, and the `[recompile] variant scan` compares per find (read
     the baseline's value first: at about 1 the switch cannot help).
3. Run once with all of them plus `APS5_VERIFY_PROOFS=1`. Expected: no abort; frames identical to the baseline
   (screenshots of the same spot).
4. Close the game window. Expected: no crash in the NVIDIA driver at exit (B7).
5. Make a new capture. Run `agc_frame_replay <capture> --summary` and check the present numbering (B8c), then
   `--png --compare` with `APS5_REPLAY_FLIP_SERIALS=1`. Expected: PSNR back to normal and frame 3 present.
   Read the per-frame `cpu ms` lines for the worker and the committer (B8b), and the `setup` time (B8a).
6. For B8d: two identical replays with `--png-all-loops`, then diff frame 1 of each loop.

### B7: exit crash

`VulkanDevice`'s teardown now, before `vkDestroyDevice`:

1. drops the statics that hold this device's buffers: the image mirrors (`Graphics::ClearImageMirrors`, new), the
   BDA table cache and the cached address space;
2. retires the buffer pool (`BufferPool::Retire`, new): the retained slots are destroyed now, and every allocation
   returned afterwards (a buffer some other static still holds, destroyed at exit) is dropped without a Vulkan call.
   The retired flag is read under the pool mutex, so a `Put` racing the teardown either retains before the drain
   or is dropped after it.

The cost of step 2: a buffer that outlives the device leaks its handles instead of freeing them (the device and
the process are going anyway). Step 1 keeps that set small.

Day-session check: start GTA V, reach the prologue, close the window. Expected: the process exits without the crash
in the NVIDIA driver (`BufferPool::Put` / `vkFreeMemory` under static destruction).

### B10: the worker's recompile phase

`Driver::compileDrawStage` returned a full copy of the capture's `RecompileResult` (SPIR-V words, binding tables,
push-constant layout) for every recompiled stage. It now returns the capture's `shared_ptr`; the draw holds them in
`compiledStages` (moved into the pipelined commit with the rest). The rare CPU-indirect path, which patches user data
and recompiles in place, copies every stage into `results` first (before, only on a draw-cache hit: a reused
data-only stage on that path would have patched `results[0]`). The snapshot hash and the variant scan are unchanged.

Expected saving: the copy, ~1-2 us of the 5.6 us recompile phase per recompiled stage (one vector of SPIR-V words
and the binding vectors; not measured here).

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` in the Lombank, compare the worker's `recompile`
column before/after (same spot, 30 s).

Second part, `APS5_VARIANT_MRU=1` (`perf(shader): most recent variant first in the recompile's variant scan`): a found
variant is swapped to the front of its source's list. At most one variant matches a (layout, specialization), so the
order only changes how many compares find it. With `APS5_PROFILE_DRAW`, a new line beside the result memo's reports
the candidates compared per find and the snapshot hash's time per call:
`[recompile] variant scan (10 s): N found, X candidates compared per find; snapshot hash Y us per call`.

Expected saving: none if X is about 1 (a source with one specialization); otherwise (X - 1) specialization compares
(two vectors of buffer and image descriptions) per recompiled stage. Not measured here: the driver tests draw too few
times for a report.

Day-session check: in the Lombank with `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1`, read X and Y without the
switch. If X is well above 1, turn `APS5_VARIANT_MRU=1` on and check that X drops toward 1 and the worker's
`recompile` column drops. If Y is a large share of the 5.6 us, the hash is the next target (for example, hashing the
descriptor words during the capture walk); keep it a full-strength hash.

Test: the `agc_*` suite with the switch on and off (same two baseline vopc failures), and the non-agc suite with the
switch and `APS5_PROFILE_DRAW=1`.

### B5: pipeline lookup identity fast path (`APS5_PIPELINE_IDENTITY=1`)

Each thread keeps the previous `CachedPipeline` lookup's key and the store entry it found. The key is built into a
reused buffer (no allocation); if it equals the previous key (one byte compare), the entry is taken without the
FNV hash and the index lookup. The entry's iterator is trusted only while the store's `generation` is the one it was
taken at; every erase from the store (eviction, abandonment of a dead device's entries, `ClearCachedPipelines`)
bumps it. The device check (`alive`) and the LRU splice run as on a normal hit, so hit counts, eviction order and
the result are a normal hit's. The `[pipecache]` line counts the repeats.

Expected saving: the key allocation and free, the byte-wise hash of the ~300-byte key and the map lookup: ~0.5-1 us
of the 1.8 us pipeline phase on repeats.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_PIPELINE_IDENTITY=1`;
compare the committer's `pipeline` column and read the repeat count in `[pipecache]`.

### B8: the frame replay as a measuring instrument

(b) done. Queue 0's worker and the pipelined-draws committer register themselves (`RegisterDriverThread`, new
`Execution/include/DriverThreadClock.hpp`: `pthread_getcpuclockid` on Linux, `GetThreadTimes` on Windows). The
replay samples both clocks at each flip and prints, per loop:

    [replay] loop N: queue 0 worker cpu ms per frame: a b c ... (T ms in F frames)
    [replay] loop N: committer cpu ms per frame: ...

These are the driver's own CPU per frame, independent of the replay's main thread. To make the tool build and run
on Linux here, the platform layer from `gta-v/hw-rt` (`d9a73a6f`) was cherry-picked (its hw-rt progress doc left
out).

Not run end-to-end: there is no game capture in this container. A capture made from `agc_driver_flip_tests`
(`APS5_CAPTURE`) records no address space (the test's command buffers are on its stack), so its replay crashes in
`applyEvent` reading them. That capture is not a valid input, and the crash has nothing to do with this change.

(a) main-thread cost. Not profiled (no game capture here), but the code shows one per-loop cost that scales with the
whole capture. Every restore copied `initialLastPage`, an `unordered_map` with an entry for every page the prologue
wrote (the base snapshot: hundreds of thousands of pages for GTA V). The restore's own rewrite of those pages also
updated `lastPage` for each of them, only for the copy to overwrite it.

Now the restore writes without touching `lastPage` (`writeRuns(..., track = false)`). The loop's own changes to it go
to an undo log that the restore rewinds, so the per-loop cost is the loop's delta pages, not the base. `writeRuns`
also reuses its two per-event vectors. Results are identical: the map ends each restore exactly as the copy left it.

Expected saving: tens of ms of setup per loop on a GTA capture (a copy of a map with ~10^5-10^6 entries, plus as
many hash updates).

Day-session check: compare the `[replay] loop N: ... setup X ms` field before and after on the same capture. For a
full profile: `perf record -g --per-thread` on the replay, then read `memory deltas written in X ms` and `pacing
waits` against the loop's wall time.

(c) flip numbering, analysis only (needs a capture to confirm). The capture numbers presents by counting
`Driver::Present` calls (`FrameCapture::NotePresent`: `event.flip = presentsSeen - flipsBefore`). The replay numbers
them by flip reservations (`ReplayOutput::Reserve`, one per FLIP packet at submission). `flipsBefore` is
`capture.flips`, the count of submitted flips. Any `Present` call that is not one submitted flip shifts every
capture index by one against the replay's: a present with no buffer from video-out setup, a repeated vblank present,
or a present of a flip submitted before the capture whose presenter ran late. The replay then pairs its flip `k` with
the capture's display buffer of flip `k±1`, which is the other swap-chain image. Dumping that image gives exactly
"~20 dB, half the pixels differ". A missing index (3) fits a present that went to a slot the replay never reached.
Made since. The capture records the flip's own serial in `PresentEvent` (the former `reserved` bytes, 48 bits: the
`FrameTiming` id the worker assigns at the FLIP packet, `++frameSerial`, which advances with `flipsCounted`).
With `APS5_REPLAY_FLIP_SERIALS=1` the replay indexes presents by `serial - 1 - flipsBefore`; presents of pre-capture
flips are skipped, and presents without a serial (older captures) keep their place. It prints how many presents
moved.

Day-session check:

1. Make a new capture (old ones carry no serials).
2. Run `agc_frame_replay <capture> --summary`. Every present line shows `present N: flip serial S (replay flip F)`;
   `N != F` is the skew.
3. Run `--png DIR --compare <live frames>` with and without `APS5_REPLAY_FLIP_SERIALS=1`. Expected: the ~20 dB
   frames reach the replay's normal PSNR, and frame 3 is dumped.

(d) frame 1 differs between identical runs: not attempted (needs a capture).

### B1: snapshot upload ring (`APS5_SNAPSHOT_RING=1`)

A new storage snapshot in `PrepareDrawBindings` takes a slice of the recorder's current arena
(`Recorder::AllocateDrawSnapshot`) instead of a `Buffer` of its own. The arena is a 1 MiB pooled host-visible
`Buffer` (`STORAGE | TRANSFER_SRC`, as before). Slices are cut forward at `minStorageBufferOffsetAlignment` (at
least 16), written once at allocation and never rewritten. Lifetime is the `shared_ptr<Buffer>` the slices share:
the draw's `DrawBindings` (kept by its batch until the fence) and the reuse entry both hold it. When an arena is
full the recorder lets go of it and takes a new one. The old arena returns to the `BufferPool` when its last batch
completes and its last reuse entry is evicted, so recycling never overwrites bytes in flight.

- The reuse rule is unchanged: same key (address, use, bytes), same generation checks. The entry now also records
  the slice's offset (`DrawSnapshot::offset`). A caller that passes no offset (the vertex/index path) is only
  served offset-0 entries.
- The descriptors take `{arena, offset, bytes}`, and `captureInputs` (APS5 input capture) copies from the offset.
- Fallbacks to a `Buffer` of its own: a snapshot over a quarter arena, or when the live arenas reach 256 MiB (an
  arena pinned by one long-lived reuse entry stays whole, so this bounds that waste).
- Not changed: the snapshot `std::map` (kept; the reuse lookup was 0.74 us and the fill's map insert is part of
  the 1.9 us fill).

Expected saving: the per-snapshot `make_shared<Buffer>` and pool `Take` (the 0.78 us "create"), about 1.6 us per
draw at 2.1 new snapshots per draw, plus the matching `Put` on release.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_SNAPSHOT_RING=1`, same spot,
30 s. Read the `create` and `fill` parts of `[draw-bindings]` and the committer's `record` column. Also watch
`[bufferpool]` for fewer small-tier hits/misses. Correctness: the Lombank prologue renders the same (A/B
screenshots).

Seen while testing: `agc_driver_recorder_tests` stopped at `unitShadowTests`. This is a lavapipe artifact, now
skipped there; the binary is a ctest since (see "Blocked, and why").

### B2: readTarget (`APS5_TARGET_PROOF_MEMO=1`)

`StorageTexture::Refresh` keeps a proof after a refresh that found the image current: the thread's collect epoch,
the unwatch serial, the pending registry's serial (read after the refresh's flush), the registry generation and
the collect generation. The next Refresh of that image returns "current" without the flush, the collects, the
alias scan or the DCC key proof if all of these hold:

- the collect epoch is the calling thread's current one, and the unwatch serial is unchanged;
- `UnchangedSince(surface, proof generation)` holds, and the same over the DCC key range;
- the pending serial and the registry generation are unchanged.

Any other outcome drops the proof.

Invalidation argument (also in the code above `refreshProved`):

- **CPU stores.** Within one collect epoch every collect of the surface or its keys returns its memoized generation
  without walking (`GuestMemory.hpp`, collect epoch). A CPU store not yet collected is therefore as invisible to a
  full Refresh as to the proof, and the next epoch (a new submission, a satisfied wait, a drain; on the committer,
  a new worker epoch token through `FollowEpoch`) drops the proof.
- **Driver stores, GPU writes into the imports and write-backs of any image there.** These stamp the blocks, which
  `UnchangedSince` sees.
- **Other images' pending results.** Marking dirty, storing, flushing or dropping any image bumps the pending serial,
  so the full Refresh's `FlushPending` and `pendingAlias` would answer as they did at the proof. Marking an
  already-dirty image dirty again does not bump it. A run of draws into the same target therefore keeps the proof
  after its first draw, which is the case being targeted.
- **The image's own pending results.** These are exempt from the flush and the alias check in the full path too.
- **DCC keys.** The proof is only taken when the key proof itself was kept. A scan made while recorded work still
  writes the keys is redone on every call, so the memo makes no proof then.
- **Host imports.** The `direct` and borrow decisions depend on host imports; the registry generation covers them.
- **The full path's other effect.** On an unchanged result it moves the layer generations to the current one. The
  memo skips that, which is safe because no stamp lies between the proof's generation and now.
- **No epoch.** A thread without one (one that never bumped, or `APS5_NO_COLLECT_MEMO=1`) never uses the proof.

Expected saving: most of the 5.1 us readTarget on draws after the first into a target within an epoch (the flush
scan, the collect memo lookups, the key proof's collect, the alias and import lookups, the vector allocations).
`UnchangedSince` over the surface and keys remains.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_TARGET_PROOF_MEMO=1`.
Compare the committer's `readTarget` column. The `[target-proof]` line (every 100k hits) gives the hit/miss counts.
Correctness: the prologue's frames match A/B (screenshots, or a replay with `--compare`).

Seen while testing: `storageRefreshTests` failed with every switch off as well. This is the same lavapipe artifact,
now skipped there (see "Blocked, and why").

### B3: resource lookup (`APS5_LOOKUP_MEMO=1`)

What was looked at: the 5.4 us lookup is key 0.95, find 0.5, `Revalidate` 2.1 and `MovedReadOnlyBuffers` 1.7.

- `Revalidate` already has the fast proof (`fastRevalidate`), the epoch gate and the pending-serial memo. Its
  per-use work (step 3, the staging copies) must run on every use.
- `MovedReadOnlyBuffers` and `DrawResourceKey` compare or hash the draw's descriptor words. A memo on them would have
  to compare the same words, so there is little to save.

The memo therefore targets what `fastRevalidate` repeats for an immediate re-use of a template by the same thread:
the collects of every surface, the DCC key proofs (a collect and an `UnchangedSince` each), the decode and the
query building. After a plain fast proof (no accepted overlap, no own-object refresh, every key proof kept, the
registry unmoved through the call) the template keeps:

- the proof's stamp queries, with the key ranges added at their proofs' generations;
- the images whose cache residency it checked;
- the thread's collect epoch, the unwatch serial, the pending serial and the registry generation.

The next `Revalidate` of that template is answered from the memo when the epoch, the unwatch serial, the pending
serial and the registry generation are all unchanged, and these checks pass:

- one `UnchangedSinceAll` over the stamps;
- the cache flags and `StorageImagesCached`, because eviction moves no serial;
- `StorageImageServesKeys` for each storage image.

Steps (2), the import checks, and (3), the staging copies, run as before.

The argument for skipping the collects and key proofs is B2's: within the epoch a collect returns its memoized
generation, and a kept key proof answers while its range is unstamped. Given an unchanged pending registry, the
proof's decisions (keys, the identities asked of the registry) come out the same.

`APS5_VERIFY_PROOFS=1` checks every memo hit against the fast proof. Use it for the day session's first runs.

A bug found by the test and fixed before the commit: the pending serial starts at 0, which is also
`pendingSerialSeen`'s "none". The memo now compares the serial directly.

Expected saving: most of `fastRevalidate` on repeats within an epoch, perhaps 1 us of the 2.1 us revalidate.
Nothing changes for templates with no textures.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_LOOKUP_MEMO=1`. Compare the
committer's lookup `revalidate` part, and read the memo hits and misses on the `[rescache] revalidate` line. Run
once with `APS5_VERIFY_PROOFS=1`: no abort is the check.

### B4: vertex and index inputs (`APS5_DRAW_INPUT_MEMO=1`)

`CopyDrawInput`'s reuse path runs the flush hook (`FlushGpuWrites`), the collect and the reuse-map lookup. Each
thread now keeps a 64-slot direct-mapped memo of the inputs it reused, keyed by (address, bytes, use, recorder). A
repeat is answered from the memo when all of these hold:

- the collect epoch and the unwatch serial are the same, so the collect would return its memoized generation;
- `UnchangedSince` holds over the range, so no driver store, recorded-GPU-write note or write-back stamped it;
- the pending serial (read after the flush) and the registry generation are the same, so the hook would store and
  publish nothing;
- the recorder has no pending write over the range and the thread queued no label over it. These are the hook's
  other two actions.

The buffer is held weakly, so the memo never keeps GPU memory alive past its reuse entry and batches, or past a
device. A hit does not touch the reuse map's LRU order.

Expected saving: most of the reuse path's cost on repeated ranges, perhaps 1-2 us of the 4.2 us vertex phase.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_DRAW_INPUT_MEMO=1`. Compare
the committer's `vertex` column. `DrawInputMemoCounters()` gives the hit/miss counts; a report line could be added
if useful.

### B6: prerequisites for making `APS5_PIPELINED_DRAWS` the default

`Driver::drawWriteRanges` now forwards to `DrawWriteRanges` (new `Execution/include/Driver/Memory/DrawWriteRanges.hpp`),
so a test can call it.

1. **Storage images.** Images that draws store to (pixel-shader image stores, image atomics) were missing from the
   ranges, so a later reader of that memory did not order against the pipelined draw. They are now included: the
   surface (`DescribeSurface(...).guestBytes`) and its DCC keys. Elements the shader only reads are left out; a
   binding without `imageWritten` flags counts as written. This is not behind a switch, because it only affects the
   pipelined path, which is off by default, and only makes it more conservative.
2. **Exact target ranges** (`APS5_EXACT_DRAW_WRITES=1`):
   - color: `[address, address + ColorTargetLayout bytes x samples x 3D slices)` instead of twice the bytes plus
     64 KiB from the surface base;
   - DCC: one key byte per 256 surface bytes instead of a sixteenth plus 64 KiB;
   - depth and stencil: `DepthSliceBytes` x samples;
   - HTILE: 4 bytes per 8x8 tile;
   - CMASK keeps its estimate (no exact formula in the code yet).

   For 1080p RGBA8 the color range goes from 17.8 MB to 8.8 MB.

Day-session check:

- `APS5_PIPELINED_DRAWS=1 APS5_EXACT_DRAW_WRITES=1` in the Lombank against `APS5_PIPELINED_DRAWS=1` alone.
  Expected: the same frames, and fewer `[draw] pipelined ... drains` caused by write-range overlaps.
- Look for any scene that writes storage images from pixel shaders; before this change it could read stale bytes
  in pipelined mode.

Still open before the default can flip: CMASK's exact size, and mipmapped color views (only the view's mip range is
used; writes go only there).

### B9: the remaining drains

**DMA_DATA** (`APS5_PIPELINE_DMA=1`). Queue 0's `DMA_DATA` to memory is now an ordered commit item, as fills and
labels already were (`Driver::enqueueDmaPacket`, called next to `enqueueLabelPacket` before the drain decision).
The commit runs under the GPU lock after the draws before it:

- **Immediate** (source 2): the depth and color metadata fill notes, then the pattern. Up to 64 KiB it is stored on
  the GPU like a label; larger, or when the GPU path refuses, it waits for the device and stores on the CPU.
- **Memory-to-memory copy**: the source is read at the commit, not at enqueue, because draws before it may write it.
  A copy over 64 KiB is recorded with `CopyBuffer`; a smaller one, or one the GPU refuses, is stored like an
  immediate (the checked `GuestMemory::Read` waits for recorded writes to the source).

The write range is the destination. These keep the drain: GDS or register selectors, overlapping copies,
inaccessible ranges, and labels still deferred.

Expected saving: the ~1.3k `DMA_DATA` drains per 10 s live. Each drain waits for the whole pipeline, so it is
worth more than the packet itself.

Day-session check: `APS5_PROFILE_DRAW=1 APS5_PIPELINED_DRAWS=1` with and without `APS5_PIPELINE_DMA=1`. The
`[draw] pipelined ... drains:` line should show no `packet 0x50` drains, and frames should be unchanged.

**Dispatch commit items: design, not implemented.** Today a non-HLE `DISPATCH_DIRECT` on pipelined queue 0 drains
(`Dispatch.cpp`, before `copyBuffer`) because:

1. its capture (user data, V#s, the memory the recompiler reads) must see the draws before it;
2. its stage A (`PrepareDispatch`, without the GPU lock) copies read-only guest buffers on the CPU, so those bytes
   must already hold the earlier draws' results;
3. its recorded work must follow the draws in the recorder.

A commit item would split it like a draw:

- **Worker:** decode, capture and recompile as now. A captured range that overlaps a pending commit's writes drains
  first (the existing `Drain(Capture)` rule draws use). Then stage A, but only if no range it copies on the CPU
  overlaps `DrawPipeline::Queue0().Overlaps(...)`; otherwise drain as today. Ranges it serves in place through
  imports are read by the GPU in recorder order, so they need no check. `noteWrittenBuffers` runs at enqueue,
  because the worker's later write evidence needs it.
- **Committer:** stage B (texture lookups, the rest of the upload, descriptor writes), the record, and the
  completion notes.
- **Write ranges:** the written buffers (`forEachWrittenBuffer`), the written storage images (as in
  `DrawWriteRanges`), and the BDA fault ranges.

Not eligible, so they keep the drain:

- address-based (BDA) dispatches: their writes are not known up front (the lease covers whole heaps);
- `DISPATCH_INDIRECT` with arguments in a pending write range;
- dispatches whose recipe path needs `Revalidate` under the lock while texture lookups happen on the worker.

What would make it not hold up: stage A's binding plan depends on texture-cache state that stage B can change
(T1 refreshes). This needs an argument like B2's, or stage A stays on the committer for dispatches with images.
Measure first how many of the ~4.7k drains per 10 s are image-free (`APS5_PROFILE_DRAW` with a counter at the drain
site).
