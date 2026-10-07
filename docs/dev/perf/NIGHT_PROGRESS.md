# night/perf progress

## Night 2026-10-07

Base: `a75aea29` (`gta-v/fidelity-main` + the pipelined-draws work). Built in `build-perf` on lavapipe; the
`agc_*` ctest suite (143 tests) passes with the switches below off and on, except `agc_driver_vopc_compare` and
`agc_driver_vopc_compare_table`, which fail at the base commit too (lavapipe).

### Commits

| Task | Commit | Switch | What the test proves |
|---|---|---|---|
| B7 exit crash | see `git log` (`fix(agc): retire the buffer pool ...`) | none (bug fix) | `agc_driver_buffer_pool_retire`: a retired pool frees its retained slots while the device lives and makes no Vulkan call for anything returned later, including a `Buffer` destroyed after the teardown (the image-mirror case) |
| B10 recompile copy | `perf(agc): share the draw stage's recompile result ...` | none (no behaviour change) | the `agc_*` draw tests pass with `APS5_PIPELINED_DRAWS` off and on; the CPU-indirect path keeps its own copies, now for every stage |
| B5 pipeline identity | `perf(agc): identity fast path for repeated pipeline lookups` | `APS5_PIPELINE_IDENTITY=1` | the `agc_*` suite passes with it on (alone and with `APS5_PIPELINED_DRAWS=1`) |
| B8 replay on Linux | `Frame replay: host memory and process calls behind a small platform layer` (cherry-picked from `gta-v/hw-rt` `d9a73a6f`) | none (tool) | `agc_frame_replay` builds on Linux here; `--summary` reads a capture |
| B8b driver CPU per frame | `feat(agc): per-frame CPU time of the queue 0 worker and the committer in the replay` | none (measurement only) | `agc_driver_driver_thread_clock`: a registered thread's clock counts its own CPU (193 ms of a 200 ms spin) and not the reader's sleep |

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

(a) main-thread cost: not attempted (it needs a game capture to profile). How to measure it: `perf record -g
--per-thread` on the replay, or the existing `[replay] loop N: memory deltas written in X ms` and `pacing waits`
fields against the loop's wall time.

(c) flip numbering, analysis only (needs a capture to confirm). The capture numbers presents by counting
`Driver::Present` calls (`FrameCapture::NotePresent`: `event.flip = presentsSeen - flipsBefore`). The replay numbers
them by flip reservations (`ReplayOutput::Reserve`, one per FLIP packet at submission). `flipsBefore` is
`capture.flips`, the count of submitted flips. Any `Present` call that is not one submitted flip shifts every
capture index by one against the replay's: a present with no buffer from video-out setup, a repeated vblank present,
or a present of a flip submitted before the capture whose presenter ran late. The replay then pairs its flip `k` with
the capture's display buffer of flip `k±1`, which is the other swap-chain image. Dumping that image gives exactly
"~20 dB, half the pixels differ". A missing index (3) fits a present that went to a slot the replay never reached.
Proposed fix (not made): record the flip's own serial in `PresentEvent` (the `FrameTiming` id the worker assigns at
the FLIP packet, `++frameSerial`, which advances with `flipsCounted`) and index by `serial - 1 - flipsBefore`; skip
presents without one.
First check for the day session: `--summary` on a game capture lists the Present events. Compare their
`flip` fields with the Submit events' flip counts.

(d) frame 1 differs between identical runs: not attempted (needs a capture).
