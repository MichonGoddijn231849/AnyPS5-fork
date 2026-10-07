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
