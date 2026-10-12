# night/merge progress

`night/merge` is `gta-v/fidelity-main` merged with upstream `main` after its prepared-shader rework (#639). The
merge commit is `5f5183e8`; the queue is section M of `docs/dev/NIGHT_SHIFT.md`.

## Day session 2026-10-11: Wolverine folded into night/merge

`night/merge` fast-forwarded to `night/wolverine-merge` (`f1ec72bca` + `dcca6f71d`): night/merge was an ancestor
(the night's W6 merges, 39 conflicts, every one kept or re-applied, are listed in `docs/dev/wolverine/PORT_PROGRESS.md`).
From now on **every game runs from `night/merge`**; `night/wolverine-merge` is retired.

- Windows-only breaks the Linux night could not see, fixed in `dcca6f71d`: the NID patch step refused libkernel
  (five Wolverine stubs export NIDs main now defines by name: `pthread_cancel`, `sceKernelSyncOnAddressWait8/16`,
  `__tls_get_addr`, `sceKernelReleaseFlexibleMemory`; main's functions stay), and `agc_frame_replay` did not compile
  against main's `GuestAllocations::Add` gpu flag (night/merge `3230cc1c` alone has the same replay break).
- Suite on Windows / RTX 5080, 745 tests: 676 pass. Not code: 63 relinker / hw-oracle Python tests (this machine's
  embedded Python does not put the script directory on `sys.path`, so sibling test modules do not import). Also failing
  on `night/merge` `3230cc1c` alone (built the same way, so not from the fold): `agc_driver_recorder`,
  `agc_driver_recorder_draw_input_ring` (a guest allocation stays pinned 60 s), `agc_driver_dcc_keys_first_write`,
  `agc_driver_wave_allocation`, `agc_driver_wave_mask_shift`. New with the fold: `agc_driver_buffer_format_compact`
  (the night's `APS5_FORMAT_SHARED_READS` / compact helpers, off by default) faults the NVIDIA device
  (`instruction pointer invalid`, then device lost); it passed on lavapipe. Needed before those switches are used.
- Next: GTA V on this head, starting with the default-on Wolverine changes (`docs/dev/wolverine/SWITCHES.md`).

### GTA V live on `1f175e3d7` (10-11 ~10:40, RTX 5080, cold shader cache, Lombank interior)

- 8.2 fps (window title), the user: worse than before; some objects black, hair missing. No crash, device loss, BDA
  fault or pin stall in the log.
- 41 skipped draws in the first ~10 minutes, all `shader info collection failed: packed pixel ancillary input has an
  unsupported live use (used by SelectU32 ? ?)`, one pixel shader family (0x25b0a6e00 among them) over six color
  targets (0x2891a0000, 0x2aee60000, 0x25dd00000, 0x27dc40000, ...); also at registration (`shader ABI hint dropped`).
  The pattern (IR after fold): the VGPR that held the ancillary word is reused as a temporary under a lane mask,
  `%128 = SelectU32 %mask, BitFieldUExtract(%40, 0, 1), %ancillary`; its readers (`IEqual32 1, %128`, more selects,
  `BitCastF32U32`) are not field extracts, so `lowerPackedAncillary` (ConstantFolder.cpp) cannot lower the word whole
  and the builtin reaches `ShaderInfoCollector`. Likely exposed by main's 93b0f48d3 (s_cselect_b64 of two masks kept
  as a lane bit), which came in with the 57f96fd2b merge; not proven. The request is in the run's stderr (first skip).
  Offline the request passes info collection and stops at the SRT runtime evaluation (no guest memory), so the live
  failure path still needs pinning down (registration or prepared-at-first-use inputs).
- The fps drop is not measured against a base yet: A/B with `ps5run\gta-headline\libs-backup-1011` (night/merge of
  10-10) in the same spot, plus one `APS5_PROFILE_DRAW=1` run.
- The user's order (10-11): Wolverine first, GTA after; the ancillary fix can be a night task.

## Night 2026-10-11

The queue is section P (GTA V on `night/merge`), then W (`night/wolverine-merge`) of `docs/dev/NIGHT_SHIFT.md`.

### Dropped in the merge (read first)

Ours that did not survive the merge of upstream `57f96fd2b`, each because main fixed the same thing its own way:

- `APS5_TARGET_KEY_PROOF` (`7f7e809b0`, a thread-local proof per DCC range in `materializeRegisterClear`): main's
  resident `TargetKeyProof` (#2655) does the same on the texture. Switch removed.
- `APS5_TARGET_PROOF_MEMO` (`4685d451c`, storage refresh proof keyed by collect epoch / unwatch / pending serial):
  main's #2683 refresh proof (`takeRefreshProof`). Switch and its miss-reason profile removed;
  `StorageTexture::TargetProofCounts` now counts main's proved / unproved refreshes.
- `APS5_NO_ALIASED_KEYS_FILL` (aliased DCC keys fill, GTA V's bloom target keys inside a 4K target): main's
  `keysFillMatches`. **Day check: watch the replay frames for a bloom that accumulates.**
- `c671ba824` (runtime image-mode switches never `OpUnreachable`): main's `EmitImage` (unreachable only outside a
  continue construct) taken whole; `agc_shader_replay` covers it.
- The packed ancillary `Whole()` test (`9905f5ddd`): main's `d8b1ad71d` rewrote `lowerPackedAncillary` and its test.
- `storageRefreshTests`' explicit `WriteBack` (our 10-10 merge fix in `tests/Recorder.cpp`): main's version of the
  same test fix kept.

### Commits

| Commit | Task | Switch | What the test proves |
|---|---|---|---|
| `0f26416a` | P0 | none (merge) | upstream main `57f96fd2b` merged; suite 718 of 718 on lavapipe |
| `1ca087f1` | P1 | none (moves saves) | `agc_driver_graphics` pipelineCacheSaveScheduleTests (fake clock): one save after a two-minute compile burst, bursts saved together, a burst that never ends saved every 5 min, small growth and no new pipelines not saved, teardown saves what grew |
| `584f9c17` | P2 (a) | `APS5_UNWAITED_CLEARS=1` | `agc_driver_recorder` unwaitedClearTests: a clear with no recorder active counts no waited clear and the next recorder's copy reads the cleared value; `--fast-clear-cost` measures clears of a 4K target |
| `56f00ec2` | - | none (fix) | `agc_driver_graphics`: the barycentric geometry stage is built once per distinct stage (same SPIR-V, another target is not answered from the memo) |
| `896c9bff` | P4 | `APS5_DRAW_INPUT_RING=1` | `agc_driver_recorder_draw_input_ring`: the recorder tests with vertex/index copies cut from the snapshot ring (copies compared by buffer and offset) |
| `5394bf59` | - | none (fix) | SPIR-V validity, found on `night/wolverine-merge` by running every agc test's modules through `spirv-val`: no `OpBranchConditional` with equal labels (44 modules, invalid from SPIR-V 1.6) and no `OpVectorShuffle` of a non-vector; suite 719 of 719. **Day check: GTA V frames unchanged (the shader cache is rebuilt once: source version changed)** |
| `5f4a7679` | - | none (size) | specialization removes the dead blocks nothing references any more: -5.8% SPIR-V words over every agc test's modules (measured on `night/wolverine-merge`, `spirv-val` unchanged), so less for the driver to compile per pipeline; suite 719 of 719. Day check: GTA V frames unchanged; pipeline compile time in the bank (`[pipeline-cache]` / first-frame stalls) no worse |
| `15bcc38e` | - | none (size) | specialization reuses one constant per type and value instead of one per folded value: -26% SPIR-V words over every agc test's modules on `night/wolverine-merge` (`agc_formatted_buffer_once`'s module 11,404 -> 1,113 words here), `spirv-val` unchanged; that test now bounds the growth of two more loads by the shared access instead of a tenth of the module. Suite 719 of 719. Same day check as above |
| `d69e6a92` | - | none (size) | specialization shares identical pure values within a block (a later equal instruction becomes a copy and folds away): -7.7% more over the test modules (5.73M -> 5.29M words on `night/wolverine-merge`), `spirv-val` unchanged; suite 719 of 719. Same day check |
| `79ee779e` | - | none (size) | specialization folds nested selects on one condition and bitcast round trips (small: -0.3% over the test modules, -1.2% on Wolverine's 0x11d049aa00); suite 719 of 719. Same day check |
| `6715f88f` | - | none (size) | value sharing along the dominator tree (an instruction equal to one in a dominating block becomes a copy): -5.6% more over the test modules (5.27M -> 4.98M words on `night/wolverine-merge`), `spirv-val` unchanged; suite 719 of 719. Same day check |
| `0baa0273` | - | none (fix) | **exactness fix for `d69e6a92` / `6715f88f`**: value sharing treated `OpLoad`, `OpPhi` and the derivatives as pure, so two loads of one pointer could merge across a store between them (and derivatives across divergent flow). Never shared now; costs the test modules 4.98M -> 5.29M words. No test caught it (no agc test reloads a pointer after a store in a dominated block); suite 719 of 719. **Day check: GTA V frames as before the night (any difference seen with `d69e6a92`..`6715f88f` was this bug)** |
| `dcefc032` | - | none (size) | loads through a pointer rooted in a UniformConstant, Input, Uniform or PushConstant variable are shared again (they cannot change within an invocation): 5.29M -> 5.24M; block-local sharing of other loads between stores was tried and saves only 0.04%, not kept. Suite 719 of 719 |
| `f504109a` | - | none (size) | `record_bda_fault` no longer stores the fault record's reserved word (zero from creation and every reset, required zero by `CheckFault`): 135 -> 126 words at every inlined fault site (-1.3% on Wolverine's 0x11d049aa00). Suite 719 of 719; the BDA fault tests read the whole record. Changes GTA V's BDA shaders only if they fault |
| `54724000` | - | none (size) | specialization removes a `record_bda_fault` call whose guard (branch on condition C) is dominated by an earlier guard on the same C: the fault function writes only while no fault is recorded, so the later call is a no-op. Found by its name. Wolverine's 0x11d049aa00: InvalidDescriptor sites 2,012 -> 689, -1.3%; `spirv-val` sweep unchanged; suite 719 of 719; `agc_spirv_optimizer_tests` (SPIRV-Tools builds only, not run here; the same module was run through the pass by hand) checks dominated, non-dominated and other-function calls. **Day check: GTA V frames unchanged; a BDA fault still reported if one is seen** |

### P1: pipeline-cache saves

The saver is its own thread (`PipelineCache::run`); it takes only the cache's own mutex, which no compile or draw path
takes. What it can block is the driver: `vkGetPipelineCacheData` may hold the driver's cache lock while it serializes
350 MB, so a save during a compile burst can stall pipeline creation. Before: a save every 10 s whenever the size
changed. Now (`PipelineCacheSaveSchedule`, no switch: it only moves saves):
- pipeline creations report to `PipelineCache::NoteCreation` (graphics pipelines, libraries and their links, the
  background LTO links, compute pipelines);
- a save happens once creations have been quiet for 3 s, at most every `SaveIntervalSeconds` (now 60), only when the
  cache grew by 8 MiB since the last save, and after 5 minutes at the latest during a burst that never goes quiet;
- teardown saves whatever grew; the file is written to a temporary name and renamed (as before);
- the `[pipeline-cache] saved ...` line now always prints, with the time spent reading the cache and writing the file
  (the 350 MB save time on the 5080 is a live check: here the cache is lavapipe's and small).

### P2: DCC fast clears

Micro test (`agc_driver_recorder_tests --fast-clear-cost`): 40 key-flip clears of a 3840x2160 RGBA8 target, Refresh
timed per clear, the collect epoch bumped per clear as the driver does per submission:
- watched memory (the live case): **median 35 us per clear** (quartiles 32-44). Of it: the collect over the surface
  ~10 us, the key scan (130 KB of DCC keys, changed by every flip) ~15 us, the recorded clear itself ~5 us. At the live
  rate (~1110 clears per 10 s) that is ~40 ms per 10 s.
- unwatched memory: ~11-14 ms per clear: #2096's guest-byte capture (5.7 ms for 33 MB) plus the texel compare. Live
  GTA memory is watched, so this is not the live cost.

(a) **Waited clears.** `Recorder::Active()` is set when the device's recorder is activated (`VulkanDevice.cpp`) and
cleared only at its destruction, so on the live path every clear is recorded; a waited clear needs no recorder (device
setup, teardown, tests). `APS5_UNWAITED_CLEARS=1` submits such a clear's batch without waiting and keeps it on the image
until its fence signals; queue order and its trailing barrier put it before any later use. The `[texture]` line counts
clears as `recorded / waited / unwaited`; the live check reads W.

(b) **Whole-surface collect.** It stamps the layers' generation for the later `UnchangedSince` checks; without it,
CPU writes made before the clear would be stamped later and read as writes after it, and the next refresh would drop the
GPU's results for them. It is memoized per collect epoch, so the upload's second collect is a memo hit. Reusing the
refresh's generation for the upload and skipping the block scan for a clear refresh (both exact) measured 36 -> 32 us
median: ~4 ms per 10 s live, not worth the subtler generation reasoning; not committed.

(c) **The clear value.** `ClearColorFor` derives the colour from the DCC key code (0000/0001/1110/1111: 0 or 1 per
channel); `ClearRegister` keys are not GPU-cleared. Nothing reads the CB registers or guest clear words per clear.

### P3

Skipped: the day session added no live profiled lines (`MERGE_PROGRESS` "Live checks (night of 10-10)" item 1).

### P4: CopyDrawInput buffers, and the replay bench itself

The bench first showed the replay worker at 1690 us per packet and the committer at 224 us per draw (10-10: ~25 and
~27). 91% of the worker was `BuildBarycentricGeometryShader`: upstream's fragment-barycentric emulation (4a2e60546,
for devices without `VK_KHR_fragment_shader_barycentric`, which includes lavapipe but not the 5080) built and optimized
its geometry stage on every draw, and its result had no variant id, which made 85% of the draws uncacheable for the
resource cache. `56f00ec2` keeps each distinct stage (keyed by its emitted SPIR-V and target) with one variant id:
worker 1690 -> 106 us per packet, committer 224 -> 101 us per draw on this machine (sampled). The bench is usable again
(still ~4x the 10-10 numbers here; the machine is not the same one, so compare only within a run of the bench).

CopyDrawInput: in the replay it is never called (`[draws] ... input copies: 0 reused, 0 ring slices, 0 own buffers` in
every window): every vertex and index input is bound in place from a host import. The committer's malloc is spread over
ShaderResources builds and small per-draw vectors, not draw-input buffers. `APS5_DRAW_INPUT_RING=1` (cut non-reused
vertex/index copies from the recorder's snapshot ring instead of a `Buffer` each: no Context copy, no pooled VkBuffer,
no control block per draw) is committed with its test but unmeasurable here; the day check reads the counter: if the
live/day replay copies inputs, the ring is the structural fix; if not, the ~13% malloc of the 10-10 profile is elsewhere.

### Day checks added

`day-checks.sh` has one check (`clears-and-inputs`: `APS5_UNWAITED_CLEARS=1 APS5_DRAW_INPUT_RING=1`; frames identical,
the clears line shows 0 waited, the `[draws]` input copies line).

The specialization changes at the end of the night (validity, dead blocks, one constant per value, value sharing,
select / bitcast folds) have no switch: the script's `base` replay, compared against the live capture's frames, is
their check (frames must still match; a cold shader cache rebuilds once, the source version changed).

### Live checks (night of 10-11)

1. **Pipeline cache** (cold cache, a run with new pipelines): `[pipeline-cache] saved ... in X ms (Y ms reading the cache,
   Z ms writing)` lines: expect a handful per run (after compile bursts, at most one a minute), not one every 10 s, and
   no stutter at the save.
2. **Fast clears** in the bank at 60 s with `APS5_PROFILE_DRAW=1`: `[texture] ... clears R recorded / W waited / U
   unwaited` (W should already be 0 without the switch) and the `images: upload clear` row of `[indirect]`.

### What is left

- P2 (b) beyond the measured 4 us: the key scan (15 us per 4K clear after a CPU key write); live keys come mostly from
  GPU fills, which are answered without a scan.
- P4: depends on the day check above.
- The replay bench numbers are ~4x the 10-10 ones on this container; the shader disk cache is cold per build (keyed by
  the source version), so a bench run after a rebuild spends ~15 minutes compiling first.

### W (`night/wolverine-merge`, written up in `docs/dev/wolverine/PORT_PROGRESS.md`)

- W6: this branch (with the P commits) merged into `night/wolverine-merge` (`e2cf7680`), all conflicts kept both sides;
  suite green on lavapipe (736 of 736 at the end of the night). The list of that branch's
  default-on changes GTA V has not been checked with is in PORT_PROGRESS (W6), for the day session.
- W1 (0x11d049aa00): 92M -> 15M inlined SPIR-V words before the merge; main's BDA span reads took it back to 37M, two
  more commits brought it to 15.4M, then the specialization changes, one fault site per lookup, a smaller fault record, a simpler compact span loop and one overflow site per formatted access and no repeated fault calls to 12.72M (the
  BDA changes behind `APS5_COMPACT_BDA_HELPERS` / `APS5_FORMAT_SHARED_READS`).
- W2 (`APS5_BINDLESS_STORAGE_TABLES`) and W4 (`APS5_STENCIL_EXPORT`) done with tests; W3 and W5 have no inputs in the
  repository.
- Found on the way and fixed on both branches: SPIR-V validity (equal branch labels, shuffle fold) and specialization
  size (dead blocks, one constant per value, shared values along the dominator tree, select / bitcast folds: -36%
  words over the test modules, 8.25M -> 5.24M). The value sharing first also merged loads across stores; fixed at the
  end of the night (`0baa0273`), see the commits table.

### P0: upstream main `57f96fd2b`

`57f96fd2b` (#2808, 10-10 15:40 UTC) is the newest commit before main's next quiet gap; 1122 commits (467 merges)
since `d70b8998`, including main's rework of shader registration (deferred / background preparation), dispatch-ahead
resolving, fixed push slots (#2661), resident target key proofs (#2655), refresh proofs (#2683), deferred SRT
faults, host-visible slab buffers and pipeline libraries. 60 files conflicted. Per file (which side survived, our
origin commit, kept / re-applied / dropped):

- ShaderInfoCollector.cpp: same change both sides (packed ancillary uses in the message); main's (ours had a comment). kept (identical).
- Submission.cpp (rewind wait loop): ours ReplayRewindAbandoned return + main noteAwaitingTitle; both kept.
- Context.hpp: ours demoteToHelperInvocation + main bufferInt64Atomics; both.
- GuestTextureResource.hpp: ours IsNullTextureDescriptor + main DescriptorSingleLevel; both.
- ShaderResources.hpp (descriptor pools): ours recycled set allocations + main updateAfterBindPools; both.
- VertexInput.hpp: includes, both.
- SrtInstructionPredicates.hpp: ours IsRawRead(op, memoryIndex) + main LoadedFromMemory; both.
- ShaderDiskCache.hpp FormatVersion: ours 21, main 24 -> 25 (both formats changed).
- Recompiler.hpp ShaderPixelStageInfo: ours quadPixelMask + main targetExportPacking/dualSourceBlend; both, ours moved last (it has a default).
- SrtEvaluator.hpp/.cpp, SrtDescriptorEvaluation.cpp: ours = the compact SRT plan (BasicEvaluator<TNode>, IrNode/CompactNode, 1f26d4ca); main = deferred faults for reads through null SRT pointers (#? d3a66dbbb/56f8a6242: InaccessibleRead, poison output, zeroed sources). Ours kept; main's re-applied on our template (InaccessibleRead in the header, ReportInaccessibleReads, the poison parameter through the compact/IR wrapper; APS5_VERIFY_SRT_COMPACT also compares the poison lists).
- DepthSurface.hpp: ours fill notes (NoteDepthMetadataFill, HtileFillClears/Covers) + main HTILE depth clear address; both.
- GuestBufferMemory.hpp: ours InPlaceReads(into)/InPlaceReadOverlaps + main DeviceReads; both.
- BufferFill.cpp includes: both.
- WrittenBuffers.cpp: ours forEachWrittenBuffer forwarding to the anonymous visitor + main appendWrittenRanges; both.
- TextureDetiler.hpp/TextureDetilerDescriptors.cpp: ours = APS5_NULL_SUBMIT reuses one descriptor set (lavapipe mapping growth, b?); main = a second (image) layout with its own pools, CMASK pools reset. Main's kept; ours re-applied per layout (firstSet/firstImageSet); the CMASK pools still reset under null submit.
- DrawCapture.cpp: ours reused-variant shortcut + main CacheableResult check; both.
- SpirvBdaLookup.cpp: ours BDA range cache globals + main defines the fault function only with a fault buffer (earlier in the function); ours kept without its own second DefineBdaFaultFunction call.
- Pipeline.hpp / ShaderValidation.cpp / Graphics Draw.cpp ValidateShaders: ours demoteToHelperInvocation, main bufferInt64Atomics as the 9th parameter; both, main's first. Pipeline.hpp fields: ours stub + main backFaces; both.
- Graphics Draw.cpp materializeRegisterClear: ours targetDccKeys (7f7e809b0, APS5_TARGET_KEY_PROOF, default on: a thread-local proof per DCC range) vs main's same optimization with the proof on the resident texture (#2655 target-key-proof). Main's kept; ours removed with its switch (same behaviour, no second proof).
- Graphics Draw.cpp resolveDrawResources: ours DrawKeyTrimmed + split-commit preparation key + lookup timer parts; main NeverReusable + DrawResourceKey with the color count. Both: the preparation key (PrepareDraw) uses main's new key signature.
- Graphics Draw.cpp record: main DrawRenderPassKey (same mix as ours plus provoking vertex) replaces our inline key; ours readsTarget over moved images and the timer parts kept; main back-face split pass + LeaveRenderPassOpen(fenced, passAccess) kept.
- Graphics Draw.cpp ShaderResources build: ours multisampled refusal + movedImages condition + main color count argument; both.
- RuntimeAbi.hpp heaps: ours sampled = ImageCapacity (256), storage 16 x 4 mip slots (b76527a9/M6, GTA V's material tables and dynamic storage images); main sampled 64, BindlessTableSlots 16 (#1789), storage 16, samplers 32. Merged: the larger of each (sampled 256, storage 64, samplers 32) plus main's BindlessTableSlots. Both sides had bumped the ABI Version to 11 for different changes (no textual conflict): now 12.
- RequestSerializer.cpp: both sides used version 14 (ours: quadPixelMask; main: targetExportPacking at 14, dualSourceBlend at 15). Main's 14/15 kept as main wrote them; ours moved to version 16 (written after main's fields). Requests serialized by our old version 14 (quad mask) are not readable as main's 14; none are in the repository (testdata has no .req on night/merge). Test (ShaderMemory.cpp): version 16, 17 unsupported.
- Recompiler.cpp specialized module: ours timing (countSpecialization) + main's failure capture; both.
- RectListShaders.cpp: ours (acb10a5d fault slot BindingNumber(TessellationControl, FaultBuffer) + the rect-list module memo) vs main's same fault slot fix; ours kept (it contains main's).
- SpirvImageEmitter.cpp EmitImage: ours c671ba824 (runtime image-mode switches never OpUnreachable: branch to the merge with OpUndef, for switches inside a loop's continue construct) vs main's fix of the same failure (OpUnreachable only outside a continue construct, joins inside). Main's kept whole (the function is identical to main's); ours superseded. Its test is agc_shader_replay (the 9 extracted requests).
- ResourceMaterializer.cpp: storage mip check same both; main's constant-swizzle mode shortcut kept; main ComputeGuardedFlatSlots + ours buildCompactPlan (after it); BindlessSlots: ours APS5_BINDLESS_SLOTS kept, its default now main's RuntimeAbi::BindlessTableSlots (16).
- ConstantFolder.cpp lowerPackedAncillary + core/libs/tests/PixelAncillary.cpp: ours = 9905f5ddd ("pixel ancillary word used whole": any non-extract use reads the word rebuilt from sample/layer), which is in main; main's later d8b1ad71d narrowed it: the word passed through a select/move/phi is rebuilt only where every reader extracts a modelled field, any other read is refused (the other bits are unmodelled, TechnicalDebt.md). Main's kept (both files equal main's). Our test Whole() (BitwiseOr32 of the word lowered) is dropped: main's Refused(BitwiseOr32) is the deliberate opposite; the case 9905f5ddd was written for (select before the sample extract) is main's Values(..., throughSelect=true). DROPPED-CHECK: a GTA V shader reading the ancillary word whole outside an extract would now be refused ("packed pixel ancillary input has an unsupported live use"): watch for it in the replay/live logs.
- State.hpp: ours (depth target htileStencil/samples; DrawKeyRegisters 48 with the sample locations 0x2fe) is a superset of main's (htileAddress; 47 with 0x005, 0x01a+4, 0x398); ours kept.
- State.cpp decodeDepth: main's DB_RENDER_CONTROL depth/stencil clear draws + ours stencil-plane condition (no stencil test, and now no stencil clear, without a stencil plane); color: ours sample-count check + main uintExport; CB_COLOR_ATTRIB check: ours removed it (MSAA decode, fragments), main relaxed it by bit 17: ours kept (no check).
- Recorder.hpp ReusableDrawSnapshot: ours offset (APS5_SNAPSHOT_RING) + main generation; both parameters (offset first).
- ShaderInputState.cpp: ours readOr + main _readHeaderPod byte count; pixel info: main's null-program export mapping + ours quadPixelMask.
- UnitShadow.cpp AnyShadowedOverlaps: ours inline vector-free walk = main's anyOverlapping (same change); main's kept.
- BufferPool.hpp/.cpp: ours Retire/Trim/RetainedBytes, destroy(allocation, live) and GPU memory counting (teardown with a retired device) + main's slab sub-allocation (TakeSlot/PutSlot, slab blocks), DeviceTierEnabled/DeviceTiered/NextCapacity. Both: a slab allocation's destroy returns its slot only while the device stands and is not counted (slab blocks are allocated uncounted); the destructor frees slab blocks only when not retired; Retire frees them while the device stands. Our no-argument DeviceBudget() (from the base) is main's DeviceBudget(memory) now.
- GuestBufferMemory.cpp: ours retireLeastRecent + importAllocation with eviction (moved below) vs main's retireImport(VkDevice, ...); ours kept, its calls use main's signature (main's importAllocation differs from the base only in that argument).
- GuestAllocations.hpp/.cpp: ours AddImage (non-releasable replay image ranges) + main's gpu flag on Add and Win32 RegisterImage/UnregisterImage; both (AddImage ranges are gpu = false).
- DirectMemory.cpp PhysicalBacking: both sides added the backing's start and size (ours for DirectMemoryMappings, main's for the views); one constructor (main's argument order), one set of members; DirectMemoryMappings (ours) + DirectMemoryViews/UnmapDirectMemoryViews (main); both.
- GuestMemory.cpp collectWrites: ours capture-dirty state + APS5_COLLECT_GRANULE_KIB granule walk (bdf5c1eee) and main's write-block widening inside a collect epoch (1a0c999ac) + walkWrites(armedOnly); both, main's widening first, then ours on its result.
- ShaderRegistry.hpp/.cpp (registration and first use): main reworked registration: shaders it cannot prepare yet are deferred (PrepareRegistered(..., &deferred), PreparedAtUse), optional background preparation (AsyncRegistrationPrepare, PlanRegistered, PrepareRegisteredInBackground, AwaitRegisteredPreparation), PrepareShaderWithDiagnostics (APS5_DUMP_SHADERS on failure), PrepareNullPixelProgram, DrawRectangle. Ours: lazy first-use preparation of any shader the registration fails on (b?/fidelity, APS5_NO_LAZY_PREPARE), the failure memo, the [prepared] profile (a02295db), SnapshotCodeHash, the replay's RegisterShaderSnapshot and publishNullPixelProgram (b19fe3e7), PreparedRectangle built from the draw's results (b19fe3e7). Merged: main's registration runs inside our catch (an exception still leaves the shader unprepared under lazy prepare, and drops a background plan); SourceHandleFor/InvocationFor take main's prepare-at-use first, then our lazy path; our first-use preparation goes through main's PrepareShaderWithDiagnostics; publishNullPixelProgram uses main's PrepareNullPixelProgram; RegisterShaderSnapshot passes main's deferred flag. The draw's rect list (Execution Draw.cpp) keeps ours (it covers stages prepared at first use, whose variant ids the registration never linked); main's DrawRectangle stays defined, unused by the draw path.
- Dispatch.cpp / PacketExecution.cpp / DispatchAhead.cpp: main = dispatch-ahead resolving of a group (resolveGroupAhead, the dispatch's resolving mode parks lookups, fromAhead adopts them; HTILE depth clear notes); ours = pipelined dispatch items (APS5_PIPELINE_DISPATCH: pipelineCandidate, reads check, drain), fill HLE as a commit item, tolerateDispatch/retryOutOfMemory wrappers, the submission collect epoch and in-flight publishing for the capture. Both: our drain and candidate only outside resolving; a candidate's reads check runs before main's adoption or lookup; main's combined 0x15/0x16 branch keeps our wrappers; main's per-submission ahead reset after our epoch logic. Added: resolveGroupAhead drains the draw pipeline first on queue 0 (only with APS5_GROUP_CAPTURE_ALL_QUEUES; ahead lookups must not validate against memory with pending pipelined writes).
- VulkanDevice.cpp: teardown: ours BdaResources table cache + address space cache drops + main's workgroup memory release; features: ours null-submit depth bounds/bias clamp + main dualSrcBlend; feature chain: ours demote + main float16; context: ours demote + main bufferInt64Atomics; compute pipeline: ours APS5_NULL_SUBMIT stub pipeline + main destroys the module after a real pipeline is created. All both.
- DepthSurface.cpp: DepthSurfaceView: ours fast-clear state (clearDepth/clearStencil, ApplyFastClear, TakeWrites) + main's HTILE depth clear (ClearDepth when the HTILE mask kernel was seen); both. DepthSurfaceTexture: ours depth plane copies (layered, integer D16 as R16_UINT, cube, base slice) + main's early returns (a T# whose extent differs, or whose format is neither the depth plane's nor a depth-bits view, is not the depth surface's: nullptr, sampled from memory); both, ours' fast-clear/take-writes first.
- Pipeline.cpp: create: ours APS5_NULL_SUBMIT stub + main's pipeline libraries (LinkPipelineFromLibraries), per-face depth-bias back-face pipeline and module release; both (the stub path skips all of main's). release: ours stub guard + main optimized/backFaces; both. pipelineKey: both made it fill a caller's buffer (ours pipelineKeyInto, main pipelineKey(key&, ...)): main's name and signature, plus main's Geometry stage as generated. Lookup: ours per-thread identity hit (APS5_PIPELINE_IDENTITY) kept; its non-identity path uses main's store scratch key.
- Recorder.cpp: ours snapshot arena slices (APS5_SNAPSHOT_RING: offset on reuse and keep) + main's in-place refresh of a changed storage snapshot (refreshDrawSnapshot, ParallelCompare); both (an arena slice is never refreshed: its buffer is the arena, larger than the slice).
- Resources.cpp: ours APS5_NULL_SUBMIT device hooks and stub pipeline, the submit thread (APS5_BATCH_SUBMITS), GPU memory accounting and out-of-memory reporting (AllocateGpuMemory) + main's Buffer capacity/usage by memory properties and host-visible slab sub-allocation; both (slab buffers are not counted: their blocks are allocated by the pool).
- ShaderResources.cpp: image record: main's `sampled` + ours null T# shortcut (IsNullTextureDescriptor, APS5_NULL_TEXTURE_PROOF); storage snapshots: ours arena offset and timing marks + main's generation (in-place refresh); descriptor writes: main's per-binding allocations (snapshotIndices) + ours thread-owned vectors, timing marks and the arena slice's offset and size in each buffer info. All both.
- Texture.cpp: ClearColorForTexel (ours) + ClearKeepsDenormals (main); both. Storage refresh proof: ours 4685d451c (APS5_TARGET_PROOF_MEMO, default on: a proof keyed by collect epoch, unwatch serial, pending serial, registry) vs main's #2683 refresh proof (keyed by generation, pending serial, key proof; takeRefreshProof): main's kept, ours removed with its switch and its miss-reason profile (one proof, not two). StorageTexture::TargetProofCounts (our test agc_driver_recorder reads it) now counts main's proved/unproved refreshes. Staging buffer keep: ours KeepTransient + main's optional staging (direct path); both. Fill cover: ours aliased DCC keys fill (APS5_NO_ALIASED_KEYS_FILL, GTA V's bloom target keys inside a 4K target) vs main's keysFillMatches (keys fills over memory other images overlap, bounded by the key extent): main's kept, ours' switch removed. DROPPED-CHECK: the bloom/DCC-keys aliasing case is now main's rule; watch the replay frames for a bloom that accumulates.
- CMakeLists.txt: ours bindless-slot ctests, agc_driver_recorder_tests in the list, null texture proof / pipelined DMA / pipelined dispatch ctests + main's new test executables in the list and the provoking-vertex timeout; the visual test takes main's ${agcDriverSources}. Both.
- tests/ShaderDeviceProfile.cpp: main's new heap checks (40 images, a full class, BindlessSlots 16, a full storage heap) with ours' rejection message for one image past the sampled heap ("metadata capacity": our sampled heap equals the image capacity, so the metadata limit is hit first).
- tests/Graphics.cpp: ours clip distance, quad pixel mask and multisample tests + main's UINT8x4 target and UINT16 export tests; both. SPIR-V inspector fields both.
- tests/Recorder.cpp: rebuilt as main's file with our changes (base..night/merge) applied hunk by hunk: WaitQueue through AcquireQueue (APS5_BATCH_SUBMITS), snapshot identity (buffer, offset) and the fresh-copy branch, snapshotRingTests, currentKeyProofTests and targetProofTests in main(). One of ours dropped: the explicit WriteBack before reading unwatched memory in storageRefreshTests (our 77c31feb merge fix of the 10-10 night) - main fixed the same test for #2096's compared images (the pending image is kept and flushed explicitly), main's kept.

Post-merge fixes (suite 13/718 failing after the textual resolution, 0/718 after):
- Build: ShaderMemory WordMask::All() (main's ShaderMemory.cpp calls it); PacketMemory noteLabelStore takes the queue (main) at our call sites; tests' GuestAllocations Add(..., gpu) (main's new argument) in LookupMemo / NullTextureProof / PipelinedDispatch; LookupMemo and NullTextureProof open their Vulkan device after the block's Release struct, so the device goes first (a pinned import lease otherwise hung the teardown for 60 s).
- State.cpp HTILE: one decode (main had added its own HTILE address line next to ours); main's test of it rewritten on a copy without the base registers (withoutBase). CB_COLOR_ATTRIB zero mask widened to ~0x3f000 (our sample/fragment fields + main's destination alpha override).
- State.cpp CMASK: main requires pipe-aligned SW_64KB_Z_X CMASK metadata for fast clears and checks the CMASK range; ours decodes the CMASK address of a multisampled target (multisampleTests). Main's requirement and range check now apply to single-sample targets; multisampled targets keep ours.
- ShaderMemory test (verifyBindlessShiftedKey, ours): main appends the guard flags (ComputeGuardedFlatSlots) after our bindless mapping block in the flattened SRT; the test now finds the mapping block before the guards instead of at the very end. The expected mapping is unchanged.
- Recorder test (main's misaligned draw region, main's patching test): read the snapshot through Snapshot::Bytes() (our arena offset). Snapshots of 64 KiB or more (main's refresh block) no longer go to the arena: they take their own buffer, so main's in-place refresh patches them (an arena slice cannot be refreshed). Smaller ones keep the arena.
- Graphics test (main's snapshot descriptor update test): our MovedBuffer holds a view of the words (not a copy), so the test keeps its words alive; the descriptor offset is checked against the snapshot's (arena slice) offset instead of zero.
- QuadPixelMask test (ours): main's fixed push slots (#2661) put the fragment stage's push constants at its own slot; the test uses PixelPushOffset/PixelPushLayout like main's other execution tests.
- PreparedShaders test (main's replaced shader header check): our ABI hint wrapper (ResolveHint, GTA V links stages whose headers do not match yet) reports and drops a failed hint, so the exported AgcDriverResolveShaderAbi no longer throws; the test calls the wrapper (no throw) and then Driver::ResolveShaderAbi directly for main's expected failure, then forgives the transaction failure as the wrapper does. ExpectFailure's message names the expected text.
- DepthSurfaceReuse test (main's): its new 16-bit raw depth bits view of a D16 surface reaches our integer plane copy (a DeviceBuffer staging buffer); the mock device gained the buffer, map and copy entry points it needs.

## Day session 2026-10-10 (RTX 5080)

The night's two day checks, on `4840fa1b`, plus a second base/split pair to rule out noise (loop 2 of the
`gta-perf-3` replay):

| run | frame ms | committer us per draw | worker us per packet | vs base |
|---|---|---|---|---|
| base (morning) | 63.4 / 47.5 / 38.9 | 27.46 | 24.93 | |
| `APS5_SPLIT_COMMIT=3` (morning) | 59.3 / 45.8 / 38.2 | 26.30 | 24.39 | 67.2 dB |
| base (repeat) | 69.2 / 49.0 / 44.0 | 29.92 | 27.26 | |
| `APS5_SPLIT_COMMIT=3` (repeat) | 63.4 / 44.8 / 38.4 | 26.92 | 24.46 | 67.2 dB |
| `APS5_DRAW_INPUT_MEMO=1` (morning) | 168.0 / 191.1 / 108.5 | 43.62 | 39.83 | identical |

- **`APS5_SPLIT_COMMIT` is on by default with 3 prepare threads** (both pairs: frame ms 4-14% and committer
  4-10% below base, frames the same). `APS5_SPLIT_COMMIT=0` turns it off; any other value sets the thread count.
- **`APS5_DRAW_INPUT_MEMO` is deleted** (frames identical, but 2-3x slower frames). Its memo table, counters and the
  `agc_driver_recorder` test are gone.
- Both checks are removed from `day-checks.sh`.
- The `agc_*` suite on the 5080 after both changes: 270 of 273 pass. `agc_driver_dcc_keys_first_write`,
  `agc_driver_wave_allocation` (`lane 0 read the old counter as 3735928559, expected 100`) and
  `agc_driver_wave_mask_shift` fail with `APS5_SPLIT_COMMIT=0` as well, so they predate today; not chased yet.

## Night 2026-10-10

The queue is section P of `docs/dev/NIGHT_SHIFT.md` (one topic: GTA V on `night/merge`).

### Commits

| Commit | Task | Switch | What the test proves |
|---|---|---|---|
| `77c31feb` | P0 | none (merge) | upstream main `d70b8998` merged; suite 533 of 535 (below) |
| `ed4b6d7e` | - | none (fix) | `agc_driver_null_texture_proof` (both) pass without `APS5_PROFILE_DRAW`; the day's "teardown hang" (below) |
| `4961acb9` | P0 | none (merge fix) | `agc_frame_replay` attaches its window before the first present (upstream #1367 moved device creation to `AgcDriverAttachWindow`); the replay runs again |
| `cf58f731` | P1 | `APS5_TRACE_TABLE_MATCH` (diagnostic) | the table-match trace prints matched offsets and immediate values; the "not a valid runtime value" chains name their immediates |
| `e8689aff` | P3 | none (merge fix) | a fast clear snapshots guest bytes only for unwatched memory (~1 GB/s of memcpy live otherwise); `agc_driver_recorder` passes |
| `a02295db` | P3 | under `APS5_PROFILE_DRAW` | the `[prepared]` line (below) |
| `6397d59b` | P2 | - | the two SRT eval memo ctest runs that tested nothing are gone |
| `c553f20c` | P4 | `APS5_SPLIT_COMMIT=<n>` | the split committer's first step (below) |
| `6b72a776` | P4 | `APS5_SPLIT_COMMIT=<n>` | posts a preparation only when the committer is 2 or more items behind; `agc_driver_vertex_fetch`: a prepared draw renders the same triangle, a draw takes its own preparation (a poisoned one shows) and ignores one made for another `State` |
| `d4a3f2ae` | P5 | none (fix) | `APS5_TRACE_TABLE_MATCH` (prints only) no longer re-keys the shader disk cache; `agc_shader_disk_cache` passes |
| `c3d5b60d` | P1 | - | the PR's test on the merge: a material-record key sampled twice binds one table |
| `night/pr-1184` `2d9a09ca` | P1 | none (fix) | on main `d70b8998`: the same test fails without the change with GTA V's live error and passes with it |

### P0: upstream main

Main at 22:20 UTC was mid-burst (dozens of merges in the hour before). The newest quiet commit is `d70b8998`
(10-09 10:46 UTC, #2112, followed by 6.1 hours without commits), the queue's suggestion too; the merged base was
`dc79e9b1` (10-08). 52 upstream merges, ~3000 lines in the driver and recompiler; no rework. Conflicts:

- `ShaderResources.cpp`: upstream #1059 (storage-image cache size) sizes the sampled-texture cache from
  `VK_EXT_memory_budget` (`[gpu] sampled texture cache budget ...` line, re-read every second) and the storage cache
  from a quarter of the largest device-local heap, at least 2 GiB, `APS5_TEXTURE_CACHE_MIB` overriding both; cache
  entries are accounted by their allocation size, and views of an evicted storage image leave the sampled cache. It
  replaces fidelity-main's `TextureCacheBudget(const Context&)` (same rule for both caches, from 10-08), which is
  removed. On the 5080 the sampled budget is now what the driver reports as free, no longer a fixed quarter: watch the
  base run's `[gpu] sampled texture cache budget` line.
- `GuestBufferMemory.cpp`: upstream #2096 (Windows host-import re-uploads) keeps Windows imports compared and leaves
  their shared aliases watched; ours keeps the imports watched by default (`0dda4bf0`, menu 2.5 -> 60 fps). Ours kept,
  their message and alias rule merged (they only apply under `APS5_WRITE_WATCH_IMPORTS=unwatch`).
- `Texture.cpp`: #2096's compared refresh (an unwatched image is compared with a snapshot of its guest bytes taken at
  upload or clear, instead of being treated as untracked) next to our `APS5_NO_UNIT_DROP`; both kept.
- `ShaderResources.hpp`, `Texture.hpp`, `CMakeLists.txt`: both sides' additions.

After the merge:
- `agc_driver_mesh_tests` (new upstream) compiles `VulkanDevice.cpp` itself and needed stb's include path.
- `agc_driver_recorder` failed twice: "the refresh's store of the results did not reach guest memory" and "(s)
  results pending when their memory left the watch did not reach it". Both read raw memory right after a `Refresh` of
  an image whose memory is unwatched. Before #2096 such an image was untracked, so every refresh stored its results
  (nothing proved the CPU had not written). Now it is compared, finds its memory unchanged and keeps the results
  pending on the GPU, as a watched image does. The two cases now write back explicitly before reading raw memory; the
  rest of each case is unchanged. On the 5080 imports stay watched, so this path runs only under
  `APS5_WRITE_WATCH_IMPORTS=unwatch`; the base run covers it.
- Suite here: 533 of 535 (`guest_sce_net`, environment; `agc_driver_recorder` before the fix). At the end of the
  night (after `6b72a776`): 531 of 533 (`guest_sce_net`; `agc_driver_vopc_compare`, an f16 NaN compare, lavapipe: it
  passed once right after the merge and fails since the container moved hosts, and main's own build fails it the
  same way).

### The `null_texture_proof` teardown hangs (DEV_PLAN section 3)

Both variants hung at exit on the 5080 and pass here. The test set `APS5_PROFILE_DRAW=1` only to read three counters.
With it on, the driver prints through `ProfileOutput`, a function-local static in the driver DLL that owns a worker
thread and joins it in its destructor. On Windows a DLL's statics are destroyed at `DLL_PROCESS_DETACH`, after the
process's other threads have been ended, and joining there can wait forever (MinGW's winpthreads). The game never
gets there (`Driver::stop` stops the output first, #2112 made that safe); a test that only opens a `VulkanDevice`
does. `agc_driver_lookup_memo`, the same test without the profile, does not hang. `RevalidateCounters()` now has its
own relaxed counters (fast, full, "no record"; two increments per revalidation), and the test no longer sets the
profile. Day check: the two tests in the 5080 suite.

### P1: #1184 on main (`night/pr-1184`)

- **What main lacks.** The test the queue asked for, built on `d70b8998`: a compute shader whose record index comes
  from two scalar loads through a pointer in the SRT, whose key is `(word >> 4) << 5` from a 12-byte record at offset
  8 (GTA V's constants, from this night's `APS5_TRACE_TABLE_MATCH` trace of the merge's replay:
  `ShiftLeftLogical32(ShiftRightLogical32(ReadConstBuffer(..., IAdd32(ShiftLeftLogical32(IMul32(IAdd32, 3), 2), 8)),
  4), 5)`). Sampled **once**, main binds it already: `MatchTableOffset` takes `x << 5` for any `x`, so the shr/shl key
  needs no new matching. Sampled **twice**, main fails with exactly the live error ("GetImageResource dword 0 is not
  a valid runtime value; chain: ... ShiftLeftLogical32(ShiftRightLogical32(...")): `TryMakeTableImage` required each
  of the entry's eight reads to have only the planned handle as user, and the second sample's T# is a second user.
  The merge's `bdace5f2` has the rule that fixes it; the three other hashes the queue named (`e081cf06`, `89ab02e8`,
  `c7f859ba`) do not exist in this repository.
- **The PR branch** `night/pr-1184` (`2d9a09ca`, on `d70b8998`): that rule alone, plus the test. Without the rule the
  test fails with the live error; with it the plan has one whole-table source and the capture and binding hold the
  table. Body: `docs/dev/perf/pr-1184.md`. `tools/check_conventions.py --base d70b8998` passes.
- **Tables beyond 16 slots: not in the PR.** The stack's #1789 candidate (`f1276346`, CouchRiv) already sizes the typed
  heaps by the runtime metadata and keeps the table's slot count as its own `BindlessTableSlots` (16). The merge's
  `b76527a9` + `b42c2469` do the same plus `APS5_BINDLESS_SLOTS`; ported onto main they conflict with #1789 and would
  duplicate it, so they are kept as a patch for a follow-up once #1789 lands (more slots are then the constant, the
  image limits and the specialization ID spacing). The day session found 64/128 slots change nothing for GTA V.
- **On the stack.** A local copy of `night/gta-main-stack` with `night/pr-1184` merged (not pushed): builds; full
  suite 512 of 514 (`guest_sce_net`, network; `agc_driver_storage_cache_budget` timed out under load beside a replay
  and passes alone in 39 s).
- **`night/merge` against the PR:** the rule is the same code (`bdace5f2`); the PR's test is on the merge too
  (`c3d5b60d`). The merge keeps `bdace5f2`'s mask stripping and ABI 11 on top, which the PR does not need.

### P2: what the merge dropped from fidelity-main

Method: every function name defined in a `.cpp`/`.hpp`/`.h` under `core/` at `e9aeb10f` (fidelity-main) that is
defined neither at fidelity-main's upstream base `e248f404` nor anywhere in `night/merge`; and every `APS5_*` switch
fidelity-main reads that `night/merge` does not. Name-level, so a function whose body changed under the same name is
not listed. The scan script is not in the repository (a regex over definitions; rerun it from the git history of this
section if needed).

| Lost (file) | What it was | Status |
|---|---|---|
| `evaluateMaterializationSources`, `srtEvalMemoMode`, `srtEvalReadsUnchanged`, `srtEvalRecordingRead`, `tableMemo` (`ResourceMaterializer.cpp`), switch `APS5_SRT_EVAL_MEMO` | the SRT evaluation memo (`b29d8b80`) | **not ported**: it measured -0.8 us per packet on the worker's capture phase on 10-08, inside the noise, and the worker is not the thread that limits the frame (the committer is). The compact plan it complemented is ported (`1f26d4ca`). Its two ctest entries (`agc_shader_memory_srt_eval_memo_1`, `_verify`) ran the shader memory tests under a variable nothing reads any more: **removed** (`CMakeLists.txt`) rather than kept passing without testing anything. |
| `EvaluateNanoseconds`, `MaterializeNanoseconds` (`ResourceMaterializer.cpp`) | the evaluate / materialize timers of the `[capture]` line (`b2f6b1c7`) | redesigned: the merged `[capture]` line still prints materialize and SRT-evaluate time (other names) |
| `BindlessTableLimit`, `SetBindlessTableLimit`, `describeShape`, `describeWords`, `hexWord` (`ResourceMaterializer.cpp`) | the pre-#639 bindless table code and its diagnostics | replaced by upstream's runtime heap metadata plus M6 (`b42c2469`, `b76527a9`) |
| `VariantMru`, `variantScanCounters` (`Recompiler.cpp`), switch `APS5_VARIANT_MRU` | most-recent-first variant scan (`f2efde33`, off by default, never adopted) | not needed: an unadopted experiment; the merged variant scan is upstream's |
| `LoadCoherentPair` (`SpirvMemoryInstructions.cpp`) | a glc `buffer_load_dwordx2` read as one 64-bit atomic load (`6beb4d94`, the BVH look-back race) | **restored** on the merge as `55447e1c`, with a hand-written look-back kernel test (`58eb55dd`) |
| `LoadFormattedSource`, `LoadGpuDescriptorFormatted` (`SpirvMemoryInstructions.cpp`) | pre-#639 upstream formatted loads (from fidelity-main's root import) | redesigned upstream (#639) |
| `insertShader` (`ShaderRegistry.cpp`) | the replay's registered-shader insertion (`58c58492`) | redesigned: the replay registers through `RegisterShaderSnapshot` (day session `b19fe3e7`) |
| `BlockSum` (`tests/execution/LookBackScan.cpp`) | the game-kernel version of the look-back test | replaced by the hand-written kernel (`58eb55dd`) |
| switches `APS5_NO_CAPTURE_REUSE`, `APS5_NO_SHADER_DISK_CACHE`, `APS5_NO_SOURCE_HANDLE_CACHE`, `APS5_VERIFY_SOURCE_HANDLE` | upstream's own pre-#639 switches | redesigned upstream (#639) |

So nothing that still applies to the prepared path is missing except the SRT evaluation memo, which is not worth its
150 lines on the numbers it had.

### P3: the live-only gap, instruments

**(a) Fast-clear uploads.** The live line `[storage] uploads by path ... clear 1110/10253.6 MiB ... by reason: keys
1110/10253.6` counts storage images re-made through the **clear path** of `StorageTexture::upload`: a surface whose
DCC keys are a fast clear is cleared on the GPU (`vkCmdClearColorImage` recorded into the open batch; the MiB are the
surfaces' sizes, not bytes copied). That path is fidelity-main's and the merge did not lose or bypass it. The replay
does it too, rarely: 28-49 per 10 s at ~12 replayed frames per second (0.3 per frame) against ~1110 at ~24 fps live
(4.6 per frame), so the live scene re-clears ~15 times as many surfaces per frame as the captured one (the capture is 3
frames, looped; its memory deltas replay the CPU's key writes, so the difference is the scene, not the replay). Each
clear costs the CPU a recorded barrier pair, a collect over the surface, and, when no recorder is active, a submit and
wait of its own (`clears N recorded / M waited` on the `[texture] ... storage images` line).

One thing the upstream merge added there tonight: #2096 snapshots the surface's guest bytes at every clear
(`captureGuestBytes`, for its compare-unwatched-memory model), which at the live rate is ~1 GB/s of memcpy on the
committer. It now snapshots only memory that is not write-watched (as #2096 itself does on the import path); watched
memory keeps fidelity-main's behaviour (`originalValid = false`). No switch: a merge fix.

**(b) Shader preparation during play and (c) the prepared invocation.** New `[prepared]` line under
`APS5_PROFILE_DRAW`, every 10 s: `InvocationFor` calls, their time, the prepared entries scanned and how many calls
did not take their first entry; `SourceHandleFor` the same; first-use preparations (count, time, failures);
registrations (`RegisterShader`, count and time); and the ABI hints (`Resolve*Abi`, count, time, dropped).

### P4: the split committer, first step (`APS5_SPLIT_COMMIT=<n>`, `c553f20c`)

What runs where now, per draw without a recipe (~97% of the replay's draws: recipes hit 231 of ~6900 per 10 s):

- **Prepare threads** (`DrawPipeline::RunPrepare`, n threads, FIFO): when the queue worker enqueues the draw's commit
  it also posts a job, `Graphics::PrepareDraw`: shader validation (the fragment outputs, through the validation
  memo), the pipeline stages, the vertex input layout and the resource cache's content key. These read only the
  draw's own compiled stages and state (the memo has its own mutex), so they need no ordering against earlier
  commits.
- **Committer**: the commit takes the preparation only when its job has finished and it was made for the same device,
  the same `State` object and the same programs (`PreparationFor`); otherwise it computes them inline as before. It
  never waits for a prepare thread. The template find, revalidation, the target refresh, the bindings and recording
  stay on it: they depend on what earlier commits did.
- The job holds the commit closure (which owns the programs it reads), so a late job never reads freed programs.
- `[split-commit] (10 s, n prepare threads): P prepared, F failed to prepare, U commits used a preparation, L came
  before theirs was done` every 10 s.

What it moves (the merge's profiled split, steady window): validate ~4.2 us, lookup.key ~1.9 us per draw, out of
~41 us (unprofiled) on the committer here. What it does not: the pipeline lookup needs the draw's `ShaderResources`
and masked state, known only at commit; the bindings (`PrepareDrawBindings`, the largest piece here, inflated by
lavapipe's descriptor calls) take the revalidation's moved buffers as input; the vertex and index copies are
dominated by their write-watch collects (5.9% of the committer's samples, against 0.8% for the copy itself), which
must happen at commit.

**Measured** (`gta-perf-3`, `APS5_NULL_SUBMIT=1 APS5_PROFILE_DRAW=1`, warm shader cache, 12 loops, loops 4-12, runs
interleaved; this container has 4 vCPUs). Committer us per draw, worker us per packet, frame ms (wall):

| Run | committer | worker | frame ms | `[split-commit]` per 10 s |
|---|---|---|---|---|
| base (3 runs) | 53.45 / 50.54 / 52.31 | 48.33 / 45.60 / 46.74 | 96.5 / 90.1 / 93.0 | - |
| `c553f20c`, n=1 | 66.32 | 66.39 | 115.0 | 4315 used, 11266 before theirs was done |
| `c553f20c`, n=2 | 64.92 | 64.70 | 114.1 | 4435 used, 9713 before theirs was done |
| `c553f20c`, n=3 | 64.71 | 64.92 | 111.8 | - |
| gated, n=1 | 53.28 | 52.33 | 97.1 | - |
| gated, n=2 (2 runs) | 54.26 / 53.55 | 53.69 / 52.73 | 98.3 / 95.9 | ~3800 used, ~450 before theirs was done, ~11500 not posted |
| gated, n=3 | 52.11 | 50.99 | 92.9 | - |

What it shows:
- The committer is not behind the worker here: in `c553f20c` 70% of commits started before their preparation (posted
  when the worker enqueued the same draw) was done, so they computed it inline anyway, and both threads lost ~13-18 us
  per draw to the job traffic (a `std::function`, a copy of the stages, a wake-up per draw) and to the prepare
  threads taking cores from them.
- Gated (this night's second commit: a preparation is posted only when the committer is 2 or more items behind), 90%
  of the posted preparations are used, but three draws in four are never posted, and the worker still pays 4-7 us
  per packet. The committer's moved phases (validate 1.4-1.5 us, the key in lookup) are below the run-to-run spread
  (±2.5 us). At best even: n=3 52.1 against 50.5-53.5 for base, frame 92.9 against 90.1-96.5.
- The whole movable part is ~4 us of ~52 us. The committer's large phases (record ~10.5 us, lookup 8.4 us,
  vertex ~7 us) depend on earlier commits or on write-watch collects that must happen at commit.

Decision: **off** (as it is by default). It does not reach 30 fps by itself on any core count. The day check runs it on
the 5080, whose CPU has more cores than this container's 4, so the prepare threads need not take the worker's and
the committer's; if the committer's us per draw does not drop there either, delete the switch. Next step if it does pay: post
preparations from the worker in batches (one wake per batch, not per draw).

`APS5_DRAW_INPUT_MEMO=1` in the same rounds: committer 53.06 / 52.13 against base 53.45 / 50.54, vertex phase 8.2 /
7.1 against 7.0 / 6.7: nothing here. Its day check stays, one replay on the 5080; if the vertex phase is not lower
there either, delete the switch.

### P5: registration (analysis only)

`splitSharedMergeBlocks` rebuilds predecessors and recomputes every analysis (dominators, post-dominators, loops)
after each split, and `splitOneSelectionMerge` recomputes them on a clone for each candidate. Batching independent
splits per round, as the fast structurizer does for loop headers (`501fb815`), is the way to cut it. Not done
tonight: every recompiler change re-keys the shader cache (a cold compile of the capture is over an hour here), and
registration is already 21 s on the 5080 with `APS5_FAST_STRUCTURIZER`. `APS5_TRACE_TABLE_MATCH`, a diagnostic
switch, was not in `ShaderDiskCache.cpp`'s `NeutralSwitches`, so setting it re-keyed every shader (it cost this night
a second cold compile); `d4a3f2ae` adds it. That commit changes the recompiler's source version, so the day's first
run compiles cold once.

The other half, shaders prepared at first use. The warm replay's `[prepared]` lines: 207 first-use preparations in
the first loop, 43 s of preparation in all (60-360 ms each; the disk cache holds compiled modules, not the
registration's analysis, so a warm cache does not help). In the capture's 5049 registered shaders:
- **The 26 LS snapshots (type 5) carry no program address**: their header has only `0x10a`, `0x10b` and `0x14a`, no
  `0x148`/`0x149` and no context registers. The draw sets the address (the HS headers, type 7, carry
  `0x148=0, 0x149=0` placeholders). `PrepareRegistered` reads `0x149` first, so each one is "left unprepared: missing
  static shader ABI register 329". Upstream main has the same code and throws there (it has no lazy path); the GTA V
  stack defers such a shader ("its program address is not in the shader registers"), which is what the merge's lazy
  path does too. The entry point is the code's start, but taking it is not enough: the LS stage's output layout
  follows the HS it is linked with, so an LS is prepared with its stage set, not alone.
- **The stage sets** are prepared by `ResolveGraphicsStagesAbi` (the game's ABI hint), which leaves a set unprepared
  when its primitive state does not decode yet ("graphics ABI left unprepared": GTA V links a hull set before the
  primitive state selects patches). Taking the patch primitive for a set that has an HS stage would prepare LS, HS
  and the domain stage at the hint, off the draw. The replay cannot measure that (it registers snapshots and sends no
  hints); the live `[prepared]` line's `first-use preparations` count is the measure (live check 1).
- The rest are vertex stages behind tessellation or geometry routing and pixel stages with interpolants:
  `PrepareRegistered` returns nothing for both at registration on purpose, since they need the draw's stage set.

### Live checks (night of 10-10)

1. **The live-only gap, one profiled run in the bank** (`APS5_PROFILE_DRAW=1`, 60 s after the first visit's compiles).
   Read, per 10 s:
   - `[prepared] (10 s): invocations N (X ms, Y us each, ... missed their first entry); ... first-use preparations P
     (T ms ...); registrations R (T ms); ABI hints H (T ms, D dropped)`. If first-use preparations or registrations
     cost more than ~100 ms per 10 s during play, preparation on the game's threads is the gap; if invocations cost
     more than ~3 us each or miss their first entry often, the prepared-entry scan is.
   - `[storage] uploads by path ... clear N/M` and `[texture] ... clears R recorded / W waited`: W above 0 means fast
     clears submit and wait outside a batch (a GPU round trip each); and the `images: upload clear` row of the
     `[indirect]` line, the clears' CPU time.
   - Against the 10-09 numbers: the clear snapshot fix (`e8689aff`) removes ~1 GB/s of memcpy that tonight's upstream
     merge added; the fps should not drop below 23.5-25 with it.
2. **GTA V on main** (a quiet day, not chased): `night/gta-main-stack` with `night/pr-1184` merged, from boot. Read the
   next `ShaderRecompiler::Recompile:` error or `[gpu] skipped` line, or none. The "GetImageResource dword 0 is not a
   valid runtime value" abort must be gone.

### Day checks added tonight (`day-checks.sh`, 2 of 8)

- `split-commit` (`APS5_SPLIT_COMMIT=3`): frames identical to base; committer us per draw and frame ms below base,
  else delete the switch (here it is even at best, above).
- `draw-input-memo` (`APS5_DRAW_INPUT_MEMO=1`, from 10-07, never checked): frames identical; the committer's vertex
  phase below base, else delete the switch (nothing here).

The fixes without a switch (`e8689aff` clear snapshots, `ed4b6d7e` revalidation counters, `4961acb9` replay window)
are covered by the base run.

### What is left

- **PR #1184**: open it from `night/pr-1184` with `docs/dev/perf/pr-1184.md` (not opened, per the queue). Then live
  check 2.
- **Tables beyond 16 slots**: a follow-up PR after #1789 lands, ported from `night/merge`'s `b76527a9` + `b42c2469`
  onto #1789's `BindlessTableSlots`.
- **The live-only gap**: one profiled bank run (live check 1) decides between preparation during play, the
  prepared-entry scan and the clears.
- **Registration and first use** (P5): batching the structurizer's splits (re-keys the shader cache; do it with a
  day's cold compile), and preparing hull stage sets at the ABI hint with the patch primitive, if live check 1 shows
  first-use preparations during play.
- **Per-draw CPU**: the split committer does not pay here (above). The committer's large phases are record
  (~10.5 us), lookup (~8.4 us) and vertex (~7 us); the sampled profile spreads over the write-watch collect (ioctl
  7%), allocation (~13% in malloc/free, much of it `CopyDrawInput`'s buffers) and lavapipe; no single function is
  above 5%.
- **The SRT evaluation memo** (P2) stays unported unless a 5080 profile shows the worker's capture phase limiting.

## Day session 2026-10-09 (RTX 5080)

`night/merge` is the integration branch from today (`docs/dev/DEV_PLAN.md`).

### Fixes

| Commit | Fix |
|---|---|
| `0dda4bf0` | Windows host imports stay write-watched. Compared imports re-uploaded and wrote back every render target at every use: menu 2.5 -> 60 fps |
| `b19fe3e7` | Rect-list stage pairs the registration never linked are built at first use (`PreparedRectangle`), instead of the draw being dropped. A thrown draw is reported again (`[gpu] skipped draw`); the merge had removed the report, so ~20 draws per frame vanished silently: the lighting composite, minimap and HUD. The replay's `RegisterShaderSnapshot` publishes the null pixel program like `RegisterShader` |
| `acb10a5d` | The rect-list control stage's fault buffer takes its own runtime-ABI slot (`BindingNumber(TessellationControl, FaultBuffer)`). Rebuilt from prepared artifacts (no binding list) it sat at binding 0 and collided with the vertex stage's buffers: "duplicate shader binding", ~45 character draws per frame dropped live only (the replay builds its rects at first use) |

Lesson: a live run must be read for `pipelined commit failed` as well as `[gpu] skipped`.

### Measurements

- **Replay** (`gta-perf-3`, loops 4-5, `APS5_PROFILE_DRAW=1`, against `gta-v/fidelity-main` `e9aeb10f`):
  - frames 20.2/21.0 dB against `live/`, the same as fidelity-main;
  - worker 23.0-23.6 us per packet (fidelity-main 23.4-24.8), committer 25.1-25.7 us per draw (26.1-27.1);
  - frame ms 57/40/34 (fidelity-main 56/39/30). No per-draw regression.
- **Live**, Lombank: unprofiled 23.5-25 fps (peaks just above 25), profiled ~22. Fidelity-main had 27.8 on 10-08,
  unprofiled. The 10-08 switches are default on both, so the gap is live-only.
  - Profiled draw thread, per packet: 24.3 us (capture 7.1, recompile 5.0, decode 3.6, key/lookup 3.2,
    lock wait 2.4).
  - Live upload profile: `[storage] uploads by path ... clear 1110/10253.6 MiB ... keys`, ~1 GB/s of fast-clear
    uploads.

### The night of 10-09's day checks

| Item | Check | Result | Decision |
|---|---|---|---|
| 0 | replay frames | 20.2/21.0 dB after the fixes | done |
| 1 | live menu and bank | menu 60 fps; bank correct at 23.5-25 fps | done |
| 4 | tolerated dispatch `0x242d49600` | it dispatches; no `skipped dispatch` in the replay or live | done |
| 7 | `APS5_NULL_TEXTURE_PROOF=1 APS5_DEPTH_TEXTURE_PROOF=1` | frames bit-identical; "no record" full walks 386 -> 0; CPU within noise | **default on** (`=0` turns off) |
| 2b | `APS5_FAST_STRUCTURIZER=1` | frames bit-identical; replay setup (registration) 166.6 s -> 21.1 s | **default on** (`=0` turns off) |
| 2 | `APS5_SPIRV_SPECIALIZE=light`, `APS5_SPIRV_VALIDATION=prepared` | one module specialized in the whole replay; frames 82.9 dB (max diff 9); no time saved | **deleted** |
| - | cold against warm shader cache | 170 s against 166 s setup: registration, not compiling, is the cost | - |
| 5 | `APS5_BINDLESS_SLOTS` 16 / 64 / 128 on `gta-fid-rtfix` | the merge replays the RT capture: 20.4 dB against `live/`, loop 1 216/162/133 ms (software RT); 64 and 128 change nothing visible (66 dB) | keep 16 |
| 3 | `APS5_SPIRV_GLOBAL_REDUNDANCY=1` | not run (comparison only) | - |

`gta-v/hw-rt` (`d8aac8c2`), RT capture:
- `APS5_HW_RT=splice-opaque` with `APS5_TEXTURE_CACHE_MIB=12000`: 307-450 ms per frame.
- `APS5_TEXTURE_TABLE_RESIDENT=1` at the default budget **fails**: ~4.4 s per frame, the table rebuild 19 times. The
  table has 4436 bindings over 4297 distinct surfaces.
- The software-RT checks (`APS5_BVH_EXACT`, `APS5_BVH_NODE_BUDGET`) were dropped: they only change the fallback.

The Wolverine checks were not run (the branch is paused).

## Night 2026-10-09

Built in `build-merge` (Release, `-DBUILD_TESTING=ON`, lavapipe). The worktree's submodules were copied from the
`night/perf` worktree (same pins; `libjpeg-turbo` is new and was initialised), and `FFMPEG_PREBUILT_DIR` points at an
unpacked FFmpeg package (the copied `ffmpeg-core` has no git metadata of its own, so its download URL was wrong).

### Commits

| Commit | Task | Switch | What the test proves |
|---|---|---|---|
| `e32ae0a9` | M1, M4, M5 | `APS5_NO_TOLERATE_DISPATCH=1` restores the abort | the 15 merge failures of M1 below pass; the prepared-shader tests run with lazy preparation and tolerance off and pass |
| `b76527a9` | M4 | none (fix) | `agc_shader_device_profile`: two dynamic storage images of one class share a heap; 17 are rejected |
| `f76b2c88` | M2 | - | the `gta-perf-3` capture (LFS) on this branch |
| `c671ba82` | M5 | none (fix) | the 9 extracted compute requests that failed SPIR-V validation compile (`agc_shader_replay`) |
| `29ee4f80` | M2, M5 | none (fix) | the replay gets past its 20th event (a snapshot registration that cannot prepare no longer aborts) |
| `b42c2469` | M6 | `APS5_BINDLESS_SLOTS=<n>` (default 16) | `agc_shader_bindless_slots_64`, `agc_driver_bindless_images_slots_64`: a 64-slot table binds and reads the right elements |
| `c1436597` | M3 | `APS5_SPIRV_GLOBAL_REDUNDANCY=1` (old pass), `APS5_SPIRV_VALIDATION=0/prepared/all`, `APS5_SPIRV_SPECIALIZE=light` | `shader_11d0310700.req` (wolverine) and GTA's largest modules compile in 1.5 GB instead of >14 GB (`agc_shader_replay`) |
| `a880a3dc` | M2, M4 | `APS5_NO_TOLERATE_DISPATCH=1` restores the abort | full suite 502/508 (same 6 failures); the rendering replay skips `0x242d49600` with its reason instead of aborting, and no longer crashes at exit |
| `501fb815` | M3 | `APS5_FAST_STRUCTURIZER=1` | byte-identical SPIR-V with and without it for 173 saved requests; structurizer and shader tests pass with it on; registration (warm cache) 242 s -> 36 s |
| `1f26d4ca` | M3 | none (merge fix) | `APS5_VERIFY_SRT_COMPACT=1` replay: 3 loops, no difference; worker 53.4 -> 49.4 us per packet; suite 502/508 |
| `eac0e300` | M3 | none (merge fix, Linux only); `APS5_NO_PROCMAP_QUERY=1` uses the maps file | replay: worker 49.4 -> 43.4 us per packet, frame 91.5 -> 85.3 ms; suite 502/508 |
| `849d99ce` | M1 | none (test data) | `agc_driver_flat_apertures`, `agc_driver_pipelined_dispatch` and `_item` pass with raw buffer descriptors |
| `3a730405` | M3 (from `night/perf` B2) | `APS5_NULL_TEXTURE_PROOF=1`, `APS5_DEPTH_TEXTURE_PROOF=1` | `night/perf`'s template proofs for null and depth textures, ported as they are (`2c509c5b`, `a161a86d`, `67825aca`, `8bf14852`): `agc_driver_null_texture_proof` gives 4 full walks without the switch and 4 fast proofs with it, here too; suite with both on 508/510 (`vopc_compare`, `guest_sce_net`, as in M1); replay (6 profiled loops, last window): "no record" 322 -> 0, full walks 297 -> 31, revalidate 58.4 -> 53.9 ms |
| `6799740e` | M1 | none (fix) | `agc_driver_image_sample_unnormalized` no longer hangs at teardown; full suite 506/508 |

### M1: tests

**Before** (the merge commit `5f5183e8` as built here, lavapipe, `ctest -j4`): the build failed one link, and of 506
tests **485 passed, 21 failed**. **After** `e32ae0a9`, `b76527a9`: **500 pass, 6 fail** (the two known lavapipe /
container failures plus four open ones below). **At the end of the night** (every commit below, 508 tests with the
two new M6 tests): **506 pass, 2 fail**, both environment: `agc_driver_vopc_compare` (lavapipe) and `guest_sce_net`
(no IPv6). The flat-aperture and pipelined-dispatch failures were the tests' buffer descriptors (`849d99ce`); the
unnormalized-sampling hang was a teardown deadlock (`6799740e`).

The link: `agc_driver_bda_device_tests`, now in upstream's test loop, links `Resources.cpp`, which calls our
`HostImportLimit()`; the test stubs it as it stubs the recorder.

| Test | Cause | Fix |
|---|---|---|
| `agc_driver_recorder`, `agc_driver_srgb8_color_target` | **merge bug**: upstream's `readDccKeys` now takes a key count; our `ProvedCurrentDccKeys` still passed surface bytes, so every `APS5_TARGET_KEY_PROOF` scan (default on) read 256x past the keys and answered Unreadable | pass the key count |
| `agc_driver_recorder` | **merge bug**: upstream's `d89076e9` (retire the host import of a range mapped again; stale GPU reads) was lost in our restructured `GuestBufferMemory.cpp` | ported: imports remember their registry range |
| `agc_driver_recorder` | no `/dev/udmabuf` in this container | the dma-buf probe is skipped when it cannot open it |
| `agc_driver_pm4` | upstream's predication; our pipelined label/DMA path enqueued `WRITE_DATA` before the predication check | predicated packets under active predication take the ordinary path |
| `agc_driver` | the pipelined label path raised an EOP interrupt for INT_SEL 3 (send data after write confirm); the ordinary path does not | same rule in both |
| `agc_driver_pm4_failure` | a label write to unmapped memory was queued to the committer, which only logs failures; upstream expects the submission to fail | labels to unwritable memory take the ordinary path |
| `agc_driver_prepared_exports`, `agc_driver_prepared_graphics` | our `quadPixelMask` read `PA_SC_AA_MASK` (0x30e/0x30f) strictly; registration-time states and upstream's tests do not set it (live queues default it to all ones) | read as its reset value. **Also live**: GTA's pixel shaders could fail registration-time preparation on this and fall to first-use preparation |
| `agc_driver_prepared_shaders` (+ `_indirect`, `_uncached`, `_debug`), `agc_driver_prepared_graphics` | upstream's contract (a missing artifact throws, a bad registration throws, a failed dispatch fails the submission); ours prepares lazily, forgives registration failures, skips the dispatch | the tests run with `APS5_NO_LAZY_PREPARE=1 APS5_NO_TOLERATE_DISPATCH=1`; registration forgives a failure only while lazy preparation is on |
| `agc_recorder_kept_bytes` | upstream test counts `vkQueueSubmit` calls synchronously; our `APS5_BATCH_SUBMITS` (default on) submits on the submit thread | test runs with `APS5_BATCH_SUBMITS=0` |
| `agc_shader_memory`, `_srt_eval_memo_1`, `_srt_eval_memo_verify` | our request serializer is at version 14 (`quadPixelMask`); upstream's test pins 13 | test expects 14, rejects 15 |
| `agc_driver_bda_device` | link (above) | stub |

**The six failures left after the first fixes, and what became of them:**
- `agc_driver_vopc_compare` (lavapipe, as on every branch) and `guest_sce_net` (no IPv6 here: `socket(AF_INET6)` =
  `EAFNOSUPPORT`). Environment only.
- `agc_driver_flat_apertures` and `agc_driver_pipelined_dispatch` (+ `_item`): passed on `night/perf`, failed here
  with their output buffer never written. **Resolved (`849d99ce`): the tests' buffer descriptors.** Both built their
  V#s with `dword3` = `0x01016fac`: `OUT_OF_BOUNDS` select 0 (structured) and stride 0. Upstream's runtime-V# buffer
  path (`SpirvRuntimeBuffer.cpp`, used for prepared shaders and BDA-using ones) checks range by that select as the
  hardware does: in structured mode an access is in range only when its offset is below the stride, so with stride 0
  every load and store was out of range, and with the buffer specialization constants set (`present` 1, the V#'s
  words) the specialized module had no access left. The static-V# path ignores the select, which is why the ~20
  other tests with the same descriptor pass. The two tests now describe raw buffers (select 3, range checked against
  `num_records`) and pass. Found with temporary traces (not committed): the capture had both buffers with the right
  words, `specializeModule` got the right constants, and the prepared artifact still had the accesses before
  specialization. **The FLAT aperture routing works on this branch** (GTA's RT stack). **Day check:** whether GTA
  binds raw buffers with select 0 and stride 0 in shaders that take the runtime path (they would read zeros and drop
  writes now); with `APS5_TRACE_DISPATCH_IO=1` such a buffer is missing from its dispatch's list.
- `agc_driver_image_sample_unnormalized` (an upstream test) hung at teardown. **Resolved (`6799740e`): a deadlock in
  our teardown, also on `night/perf` and `gta-v/fidelity-main`.** `~State` took the queue lock (`AcquireQueue`) for
  the whole teardown and, still holding it, published the unit shadows and synced the recorder; with
  `APS5_BATCH_SUBMITS` (on by default) that sync's batch goes to the submit thread, which needs the same queue mutex
  to submit it, so the fence never signalled (`[gpu] recorded batch still running on the GPU after 5 s ...`). Passes
  with `APS5_BATCH_SUBMITS=0`, which pointed at it. The lock now covers only the `vkDeviceWaitIdle` it was taken for.
  **For `gta-v/fidelity-main`:** the same three-line change (any device teardown with unit shadows pending can hang
  there, e.g. at game exit).

### M2: the replay on the merged driver

The capture is on the branch (`f76b2c88`, LFS). What it took to replay all its frames:
- `29ee4f80`: the capture registers its shaders through `RegisterShaderSnapshot`, which threw on the first shader the
  registration could not prepare (event 20). It now forgives the failure like `RegisterShader`.
- `c671ba82`: 9 compute shaders failed SPIR-V validation at registration (see M5).
- `c1436597`: SPIRV-Tools' global redundancy elimination copies its value table per dominator-tree level; on GTA V's
  largest modules (and Wolverine's `11d0310700`) one compile took over 14 GB and the container's memory limit killed
  the replay three times. The default is now the local pass (`APS5_SPIRV_GLOBAL_REDUNDANCY=1` restores the global
  one); the same request compiles in 1.5 GB.

`APS5_NULL_SUBMIT=1 APS5_PROFILE_DRAW=1 agc_frame_replay testdata/gta-perf-3 --loop 3 --hidden` then ran all three
loops with **no skipped draw or dispatch** (no `[gpu] skipped` line), no command mismatches, 4018 draw packets per loop
(4006 committed, 1335 per frame; `night/perf` replays 1343 per frame of this capture, 8 more: not explained yet, see Left).
16 LS-stage snapshots are "left unprepared" (register 329, M5) and prepare at first use without a skip.

**The rendering replay** (`--png --compare testdata/gta-perf-3/live`, no null submit) cannot finish in this container:
- lavapipe has no subgroup operations in vertex shaders, so draws whose vertex stage uses them fail ("subgroup
  capability 61 is unsupported for shader stage 1"; skipped, `[draw] pipelined commit failed`). The 5080 supports them
  in every stage.
- As a consequence a depth surface is never drawn, and compute shader `0x242d49600` then reads it as a 32-bit integer
  texture ("... depth-layout texture 0x246b40000, which is no depth surface drawn with, is not implemented"). That
  error **aborted the replay** (and would abort the game): `tolerateDispatch` let errors from `Graphics` through.
  Fixed in `a880a3dc`: any error of a named compute shader skips the dispatch with its reason, as draws do.
- Then an address-based build needs `0x23dc00000+0x120f00000` (4623 MiB) visible to the GPU: lavapipe refuses the host
  import (`vkCreateBuffer` -2) and the copy fallback is capped at 64 MiB (`APS5_ADDRESS_COPY_MAX_MIB`), which ends the
  run (`FATAL`). A container limit; the day machine imports it.
- The first run of it also crashed at exit (`double free or corruption`): the static `Driver`'s destructor clears the
  texture caches, which are function-local statics created after it and so destroyed before it. The three caches
  (`Textures`, `StorageTextures`, `NullTextureCache`) are now never destroyed (`a880a3dc`). This is the same class as the
  `SubmitThread` exit hang `gta-v/fidelity-main` fixed.
- The null-submit and rendering runs use separate disk-cache entries (null submit targets the day machine's 32-wide
  subgroups), so the first rendering run compiles everything again.

So no PNG comparison was possible here; it is the first day check.

### M3: the 2.5 fps

**Measured here** (`APS5_NULL_SUBMIT=1`, the defaults, which include last night's seven switches):

| | loop 0 (cold) | loop 1 | loop 2 |
|---|---|---|---|
| setup | 868.9 s (shader compiles, disk cache cold) | 2.6 s | 1.7 s |
| frame ms | 34577 / 2179 / 572 | 124 / 111 / 143 | 102 / 107 / 91 |
| worker us per packet | - | 59.4 | 52.2 |
| committer us per committed draw | - | 62.3 | 53.2 |

Committer phases (us per draw, from the last 10 s window, which also holds the end of loop 0): validate 4.4, vertex
9.5, setup 0.8, readTarget 14.5, prepare 0.5, lookup 14.4, resources 11.3, pipeline 4.5, record 15.5, keep 2.2.

- Draw-time specialization (`[specialize]`, new in `c1436597`): 264 specialized modules, all in loop 0, 16.3 s in total
  (the longest 4.9 s, synchronously inside a draw); **none in loops 1-2**. So once every specialization set has been
  seen, specialization costs nothing per draw, but in a live game each new set stalls the draw that meets it for up to
  seconds. `APS5_SPIRV_SPECIALIZE=light` (a shorter pass list for specialized modules) and
  `APS5_SPIRV_VALIDATION=0|prepared` (the default, `all`, validates every module, specialized ones included, as
  before; `prepared` skips the validation of specialized modules) shorten that stall.
- Steady state, against `night/perf` **in this container** (one 6-loop run each, loops 1-5; `night/perf` with the
  seven switches set by hand, since they are off by default there):

  | | worker us per packet | committer us per draw | frame ms (loops 1-5) | batches per frame |
  |---|---|---|---|---|
  | `night/perf`, seven switches | 41.3 (+-1.1) | 52.0 (+-4.0) | 67-125, mostly 75-95 | 40.7 |
  | `night/merge`, defaults | 57.0 (+-2.4) | 59.8 (+-2.3) | 81-121, mostly 92-117 | 44.5 |
  | `night/perf`, no switches | 65.5 (+-1.6) | 71.2 (+-3.3) | ~135 | 65.1 |

  Before the two merge fixes below, the merge was ~38% slower on the worker and ~15% on the committer than
  `night/perf` with the same switches, and its frames ~20% longer. Not 24 times slower: the replay does not reproduce a 2.5 fps steady state. (This container is
  slower than last night's: `night/perf` with the switches read ~41 / ~45 us there.)
- **The likely live cause, fixed in `e32ae0a9`**: a shader whose first-use preparation failed was prepared again
  (decode, plan, compile attempt, under the snapshot's mutex) at **every draw** that used it, then the draw was skipped.
  The live log listed ~70 shaders left unprepared at registration; any of them failing again at first use cost a
  full recompile attempt per draw, which matches "~1.7 cores busy and nothing left to compile". Failures are now
  remembered per code offset and key. The replay cannot show it: in the replay every first-use preparation succeeds.
- **Registration costs minutes even with every shader in the disk cache.** Loop 0's setup (the capture's
  `RegisterShaderSnapshot` calls) took 247.7 s with 2256 disk-cache hits and 9 misses. gdb stacks of the main thread
  (8 of 8 samples) are all in the structurizer, which `PrepareShader` runs (`getSource` -> `PrepareResourceProgram`)
  before any disk-cache lookup:
  - `isolateSemanticLoopHeaders` gives each loop header with instructions an empty block in front of it, one header at a
    time, and after each one recomputes every analysis (dominators, post-dominators, back edges, loops, SCCs);
  - then `splitSharedMergeBlocks`, also one recompute per split;
  - and each recompute's `computePostDominators` is a set-intersection dataflow over all blocks that walks them in
    forward order (the slow direction for a backward problem) and re-sorts each set after adding one id.

  `APS5_FAST_STRUCTURIZER=1` (off by default): the headers are isolated in one pass, highest block id first (an
  isolation renumbers only the blocks at or after its header), with one recompute after the pass; and post-dominators
  are iterated in reverse block order with a sorted insert. Both give the same graph: the same fixed point of the same
  dataflow, the same blocks inserted at the same places. Checked: byte-identical SPIR-V (or the same failure) for all
  163 Wolverine requests and the 10 GTA requests saved here, with the disk cache off; the structurizer and recompiler
  tests pass with the switch on. The switch is in the disk cache's neutral list (same output). In the live game this is
  load-time and first-use time: registration on the game's thread, and `LazyPrepare` inside the first draw.

  **Measured** (loop 0's setup, warm disk cache, one run each, `APS5_NULL_SUBMIT=1`):

  | | setup (registration) | disk cache |
  |---|---|---|
  | off | 242.2 s (247.7 s in an earlier run) | 2256 hits, 9 misses |
  | header isolation in one pass only (first version) | 135.9 s | 2270 hits, 9 misses |
  | `APS5_FAST_STRUCTURIZER=1` (both changes) | **36.0 s (-85%)** | 2270 hits, 9 misses |

  *Expected on the day machine:* the same share of the game's shader registration and first-use preparation (the
  structurizer is plain CPU work; the 5080 box's CPU is faster, so the absolute times are smaller). *Day check:* start
  the game twice to the main menu with a warm cache, with and without the switch, and compare the time to the menu
  and the hitches when entering a new area; then the replay's frames with the switch (they must not change). If
  both hold, it can become the default.
- **Where the steady-state gap comes from** (20 loops each, both with `APS5_PROFILE_DRAW=1` and `APS5_CPU_SAMPLE=1`,
  loops 1-19; `night/perf` with the seven switches):

  | | worker us per packet | committer us per draw | mean frame ms |
  |---|---|---|---|
  | `night/perf` | 40.3 | 47.8 | 82.1 |
  | `night/merge` | 53.4 | 54.0 | 95.5 |

  Worker phases (us per packet, perf -> merge): decode 4.8 -> 7.6, capture 9.6 -> 15.6, the rest within ~0.6.
  Committer: lookup 8.0 -> 10.5, the rest within ~0.5. In the worker's CPU samples the merge spends more in:
  - the SRT walk of the resource capture: 184 samples in `evaluateRuntimeSources<IrNode>` against 86 in
    `evaluateRuntimeSources<CompactNode>` on `night/perf`. **A merge loss:** `gta-v/fidelity-main`'s `d503969a` and
    `19484c36` (the walk over a compact, dense copy of the plan) survived in the SRT walker, but the merge took
    upstream's `ResourceMaterializer.cpp`, which no longer numbers the plan's values densely nor builds the compact
    copy, so every walk fell back to the IR. **Ported back** in `1f26d4ca` (no switch: it restores fidelity-main's behaviour). With
    `APS5_VERIFY_SRT_COMPACT=1` (every compact walk checked against the IR walk; now a cache-neutral switch) three
    loops ran with no difference and no skip. 20 loops after the port: worker **53.4 -> 49.4 us per packet**, its
    capture phase 15.6 -> 10.7 us, the walk 184 -> 90 samples (now `CompactNode`), committer 54.0 -> 52.3 (noise),
    mean frame 95.5 -> 91.5 ms. *Expected on the day machine:* the same ~5 us per draw packet less on the worker
    (fewer in absolute terms on the faster CPU). *Day check:* `APS5_VERIFY_SRT_COMPACT=1` for a few minutes of play
    (an exception names any difference), then the frame rate without it.
  - `DecodeVertexStageInfo`: 99 samples against 48, from the same one call per draw (`Draw.cpp:304`); its own code
    barely changed, the difference is its `GuestMemory::Read`, and in that `CheckRange`: 83 samples against 18.
  - `GuestMemory` verify / `CheckRange`: 92 / 75 samples against 28 / 24, and `ioctl` 8.5% of the worker. **A merge
    loss on Linux only:** upstream's `describePages` answers a query outside the cached page spans with a
    `PROCMAP_QUERY` ioctl and does not store the answer, where our `/proc/self/maps` path (and the Windows
    `VirtualQuery` path) store each readable run in the page cache. So every check of such a range paid an ioctl.
    With `APS5_NO_PROCMAP_QUERY=1` (the maps path) the merge already matched `night/perf` (worker 41.6, committer
    47.1, frame 81.9 ms). Fixed in `eac0e300`: the ioctl path stores its runs in the page cache as the other two do (forgotten again
    if the allocation generation moved during the query). 20 loops after the fix: worker **49.4 -> 43.4 us**,
    committer 52.3 -> 48.6, mean frame **91.5 -> 85.3 ms**, `CheckRange` 83 -> 15 samples. The day machine, if it is
    Windows, never had this cost.

  With the compact plan and the page cache, the merge's steady state in this replay is within the noise of
  `night/perf` with the same switches (worker 43.4 against 40.3, committer 48.6 against 47.8, frame 85.3 against
  82.1 ms). Without `APS5_PROFILE_DRAW` (which itself costs ~15%, see `night/perf`'s B2) and with
  `APS5_FAST_STRUCTURIZER=1`, 20 loops: worker 38.3 (+-6.6) us per packet, committer 43.6 (+-6.9) us per draw, mean
  frame 76.3 ms, against `night/perf`'s 33.5 / 41.0 / 69.6 ms: ~10% behind, with a wider spread.

### M4: heap capacity at first use

**Never an abort** (`e32ae0a9`): a compute dispatch whose shader cannot be prepared ("compute shader 0x...: ...") is
now a skipped dispatch with its reason (`reportSkip("dispatch", ...)`), as draws already were.
`APS5_NO_TOLERATE_DISPATCH=1` restores the abort; the prepared-shader tests run with it.

**Why `0x242d49600` overflowed** (`b76527a9`): not the sampled heap. `ShaderInfo::MaxImages` is 64, so no group of
sampled images could exceed the sampled heap's 64. The storage heap could: `StorageHeapCapacity` (4) was also the
mip-slot count of one dynamic storage image (`ImageMipMode::DynamicStorage` takes 4 consecutive entries), so a compute
shader with **two dynamic storage images of one class** (a mip-chain generator writing two levels, which is what the
`0x242d4xxxx` family next to it is) needed 8 entries in a heap of 4. The mip slots are now `StorageMipSlots` (4) and
the storage heap holds 16 dynamic images; the sampled heap's limit is the metadata's `ImageCapacity`. Descriptor
layouts are sized from the compact binding count, so nothing grows for shaders that fit before. Test:
`agc_shader_device_profile` (two dynamic storage images of one class share a heap; 17 are rejected).

### M5: what the registration cannot prepare

- Registration now forgives a failed preparation on the snapshot path too (`29ee4f80`; `RegisterShaderSnapshot`, used
  by the replay), as `RegisterShader` does: the capture aborted the replay at its 20th event otherwise.
- **A failed first-use preparation is remembered** (`e32ae0a9`): `LazyPrepare` cached successes only, so a shader whose
  preparation failed reran `PrepareShader` (decode, plan, compile attempt) under the snapshot's mutex at **every draw**
  before the draw was skipped. Failures are now kept per code offset and static key (at most 256 per snapshot) and
  rethrown at once. On the day machine this removes a per-draw compile attempt for each of the ~70 shaders the
  registration leaves unprepared, if their first-use preparation fails too.
- Successes were already cached (`entries.push_back` after `TryCreate`), so a lazily prepared shader is built once.
- What the replay's registration leaves unprepared (3 loops of `gta-perf-3`):
  - 9 compute shaders (`0x242d47200`-`0x242d48a00`, `0x242d5fe00`, `0x242d60800`): **SPIR-V validation failure**
    ("continue construct not structurally post dominated by the back-edge block"). Upstream's runtime image-mode
    switch ends its default and each inactive mode in `OpUnreachable`; inside a loop's continue construct that breaks
    post-dominance. Fixed (`c671ba82`): those edges branch to the switch's merge with an undefined result.
    The extracted requests (`agc_shader_replay`) compile after the fix.
  - 7 LS-stage vertex shaders (`0x25a5e1400`, ...): "missing static shader ABI register 329" (0x149,
    `SPI_SHADER_PGM_HI_LS`): the registered state of a tessellation LS shader does not carry its program address;
    they prepare at first use.

### M6: large material tables on the runtime ABI

`APS5_BINDLESS_SLOTS=<n>` (`b42c2469`; default 16, clamped to 2..128): the slots a bindless image table binds. Design: the table
stays in the sampled heap of its image class (the compact layout sizes the descriptor array from the binding count, so
no separate descriptor array is needed), and the static interface grows with the slot count. What had to move:
- `ShaderInfo::MaxImages` 64 -> 256 (the expanded image count; `ImageCapacity` of the runtime metadata). Direct images
  keep the old limit (`MaxDirectImages` = 64), so shaders without tables are unchanged.
- Specialization constant IDs: `ImageModeBase + resource * 256` reached `CompareBase` (49152) at resource 64, and
  `DescriptorIndexStride` (128) would let a 256-element heap overlap the next binding. `CompareBase` moves to 131072 and
  the stride to 256.
- The disk cache keys on every `APS5_*` variable the recompiler reads (`Generated::RecompilerSwitches`), so artifacts
  built with another slot count are not reused.
Cost: each extra slot adds 48 bytes of runtime metadata to the shader's per-draw `ShaderData` (64 slots: ~3 KB more per
draw of such a shader). Tests: `agc_shader_bindless_slots_64`, `agc_driver_bindless_images_slots_64`.

### For the day session (RTX 5080), in order

0. **The replay's frames** (M2). `agc_frame_replay testdata/gta-perf-3 --loop 3 --png <dir> --compare
   testdata/gta-perf-3/live` on the 5080: no `[gpu] skipped` line, no `pipelined commit failed`, and the PSNR of
   `night/perf`'s runs. The container could not render it (lavapipe, see M2).
1. **The live frame rate** (M3). Run the game on `night/merge` with the defaults to the main menu and the bank. Expected:
   well above 2.5 fps once the first frames' compiles are done (`e32ae0a9` stops the per-draw re-preparation of
   failed shaders). If it is still low, run with `APS5_PROFILE_DRAW=1` and read: the `[specialize]` lines (modules
   built per 10 s and the longest; they should stop once the scene's sets are seen), the `[gpu] skipped` lines (a
   shader that keeps failing), and the `[draw]` / `[dispatch]` phase lines against `night/perf`'s.
2. **Specialization stalls.** Compare the first-visit hitches in a new area with and without
   `APS5_SPIRV_SPECIALIZE=light` and `APS5_SPIRV_VALIDATION=prepared` (both together, then each). The `[specialize]`
   line's total ms should fall. Then check the frames for differences with `light` (a less optimized module must
   render the same).
2b. **Registration and first-use preparation** (`APS5_FAST_STRUCTURIZER=1`, M3): time to the main menu with a warm
   cache with and without it (242 s -> 36 s of registration in the replay here), the hitches on entering a new area,
   and the replay's frames with it. Expected: much shorter loading, identical frames.
3. **Large shaders.** `APS5_SPIRV_GLOBAL_REDUNDANCY=1` brings back the global pass: only for a comparison of GPU time on
   a heavy scene, not for play (it took over 14 GB on the largest modules here).
4. **The tolerated dispatches** (M4): `0x242d49600` should now dispatch (no "shader image heap capacity exceeded"),
   and any `[gpu] skipped dispatch` line names a shader to look at next.
5. **Material tables** (M6): `APS5_BINDLESS_SLOTS=64` (then 128) where `night/perf` needed #1184's large tables; look
   for missing or wrong textures with the default 16.
6. **Buffer range select** (M1, `849d99ce`): the runtime-V# path drops accesses through a structured (select 0) V# of
   stride 0, as the hardware does (KytyPS5 implements the same rule: `zero_stride_oob`, "bounds mode 0 checks
   offset >= stride, so zero stride makes every vector read out of bounds"); the static-V# path ignores the select
   and is the lenient one. Nothing to fix unless a GTA shader relies on the lenient reading; a buffer missing from a
   dispatch's `APS5_TRACE_DISPATCH_IO=1` list would show it.
7. **Template proofs** (`3a730405`): item 0's frame comparison again with `APS5_NULL_TEXTURE_PROOF=1 APS5_DEPTH_TEXTURE_PROOF=1`; the frames must match the run without them (shadows, depth of field and SSAO sample depth), and `[rescache] revalidate` should show no "no record" full walks.

**Left:**
- M3's remaining steady-state gap to `night/perf` (~10% unprofiled, see the end of M3);
- the 8 draws per frame the merge commits fewer than `night/perf` on the same capture (1335 against 1343): none fails
  (`APS5_TRACE_DRAWS=1` names no failed draw), so they end as "nothing to draw" in the precheck, which now judges color
  writes by upstream's `Graphics::ColorWriteMask` (target state included) instead of `CB_TARGET_MASK & CB_SHADER_MASK`;
  draws with no effective color output and no depth/stencil write are skipped. Probably right (the hardware writes
  nothing for them); the day session's frame comparison (item 0) is the check;
- the rest of the registration cost (`splitSharedMergeBlocks` recomputes per split);
- **other fidelity-main code the merge dropped from `ResourceMaterializer.cpp`** (it took upstream's file; 732 of
  fidelity-main's lines are gone): besides the compact plan, the SRT evaluation memo (`APS5_SRT_EVAL_MEMO`; the
  variable is now read nowhere, so `agc_shader_memory_srt_eval_memo_1` / `_verify` pass without testing it), the
  evaluate / materialize timers, and the bindless table code (replaced by M6). Other files the merge resolved to
  upstream's version may have lost fidelity-main work the same way. A scan of every `.cpp` under `core/` for functions
  fidelity-main defines and the merge no longer does (most hits are upstream renames of unknown NIDs and moved export
  files) leaves these in the driver and recompiler. Checked, and redesigns rather than losses: `ShaderRegistry.cpp`'s
  `FailureMemo` (fidelity-main's failure memo in the old `SourceHandleFor`; `e32ae0a9` re-added one in the prepared
  registry), `Pm4.cpp`'s `Gds` (upstream maps GDS memory, `GdsAddress`), `State.cpp`'s `PixelProgramUnset` (now
  `PixelProgramSkipped`), `RdnaVectorOpDecoder.cpp`'s `usesSignedSaturateClamp` (generalized upstream to
  `usesIntegerSaturateClamp`). Not checked: `DescriptorBindingBuilder.cpp` (`GuestImagesDescriptor`,
  `ImageShapeFor`: bindless code M6 replaced), and the removed tests `tests/Recompiler.cpp` and
  `tests/execution/FlatGlobalLoads.cpp`;
- the LS-stage snapshots without register 329 (they prepare at first use).
- the day check of the two template proofs ported from `night/perf` (see the commits table; its `NIGHT_PROGRESS.md`, B2, has the check: frames the same with them on, fewer "no record" full walks).
