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
| B1 snapshot ring | `perf(agc): draw snapshots as slices of shared arenas` | `APS5_SNAPSHOT_RING=1` (`APS5_SNAPSHOT_RING_KIB`, default 1024) | `agc_driver_recorder_tests` (not a ctest on this branch; run by hand) `snapshotRingTests`: slices aligned for a storage descriptor, non-overlapping, one arena until full, a new one after, over a quarter arena refused, an arena released with its last slice. The reuse and in-flight tests there now check slices by (buffer, offset) and copy from the slice's offset; they pass with the switch on and off. The `agc_*` suite passes with it on (also with pipelined draws). |
| B2 target proof memo | `perf(agc): memoize a storage image's refresh proof within a collect epoch` | `APS5_TARGET_PROOF_MEMO=1` | `agc_driver_recorder_tests` `targetProofTests` (runs with the switch set): a repeat within the epoch is answered by the proof; a stamp over the surface, a new epoch after a CPU store, and another image marked dirty over it each force the full Refresh (which then uploads for the CPU store). The `agc_*` suite passes with it on (also with pipelined draws). |
| B3 lookup memo | `perf(agc): re-prove a reused template from its last fast proof` | `APS5_LOOKUP_MEMO=1` | `agc_driver_lookup_memo` (sets the switch and `APS5_VERIFY_PROOFS=1`, which runs the fast proof beside every memo hit and aborts on a disagreement): a repeat within the epoch is answered by the memo; a pending-registry change, a new collect epoch and a registry mutation each force the fast proof; results never change. The `agc_*` suite passes with it on. |
| B4 draw input memo | `perf(agc): per-thread memo of reused draw inputs within a collect epoch` | `APS5_DRAW_INPUT_MEMO=1` | `agc_driver_recorder_tests` `drawInputMemoTests` (with the switch set): a repeat within the epoch is answered by the memo (same buffer, same derived value); a stamp, a new epoch after a CPU store, a pending-registry change and a recorded pending write each force the full path. The `agc_*` suite passes with it on, and with all B switches on together. |
| B6 pipelined-draw prerequisites | `fix(agc): storage images and exact target ranges in the pipelined draws' write ranges` | storage images: none (a fix to the pipelined path, which is itself off by default); exact ranges: `APS5_EXACT_DRAW_WRITES=1` | `agc_driver_draw_write_ranges`: a storage image a draw stores to is a write range (surface and keys), one it only reads is not, one without flags counts as written; the exact color/DCC/depth/stencil/HTILE ranges hold every addressed texel and nothing past the layout; the estimates cover the exact ranges; 4-sample targets scale. |
| B8c flip numbering | `feat(agc): record each present's flip serial; replay by it under a switch` | `APS5_REPLAY_FLIP_SERIALS=1` (replay); the capture always records the serial in the event's reserved bytes | not tested end to end (no game capture here; the flip test's fake output never calls `Driver::Present`). `--summary` now lists every present's Present-count index beside its flip serial |
| B8a replay main thread | `perf(agc): replay restores its page map from an undo log` | none (tool; same results) | builds on Linux; not run end to end (no game capture here) |
| B9 DMA_DATA commit items | `perf(agc): pipelined DMA_DATA as ordered commit items` | `APS5_PIPELINE_DMA=1` (with `APS5_PIPELINED_DRAWS=1`) | `agc_driver_pipelined_dma`: an immediate fill and a copy of it in one DCB land as the CPU path stores them, in order (the copy sees the fill), both committed, none drained. The `agc_*` suite passes with it on, with and without pipelined draws. Dispatch commit items: design only (below) |

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

Pre-existing, seen while testing: `agc_driver_recorder_tests` stops at `unitShadowTests` ("the partial publish did
not copy exactly the partly covered unit", then SIGSEGV), with and without the switch and at `e302895b` without
B1. That is probably why the binary is not a ctest here.

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

Pre-existing, seen while testing: with `APS5_NO_UNIT_SHADOW=1` (to get past `unitShadowTests`),
`agc_driver_recorder_tests` stops at `storageRefreshTests` ("a CPU store into a unit with results pending was not
seen by the refresh"), with every switch off too. The binary is not a ctest on this branch.

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
