# night/wolverine-merge progress

The queue is section W of `docs/dev/NIGHT_SHIFT.md` (night of 2026-10-11, after section P on `night/merge`).

## Night 2026-10-11

### Summary for the morning

- **W1 (0x11d049aa00 size)**: 92.3M inlined SPIR-V words at the start, 15.0M before the merge, 36.9M right after it
  (main's BDA span reads), **12.72M** now (both `APS5_COMPACT_BDA_HELPERS` and `APS5_FORMAT_SHARED_READS`; the module
  passes `spirv-val`). The README metric (main + helpers x calls) is ~3.8M against the 1.5M goal: main's own 1.53M is
  above it alone (by its instruction patterns, roughly a quarter of that is main's per-operand float denormal / NaN
  emulation: `Select(exponent == 0, sign, x)` and the masks around it). Needs the live compile check.
- **W2**: stores and atomics through bindless tables behind `APS5_BINDLESS_STORAGE_TABLES` (slot path, 16 slots).
- **W4**: stencil export behind `APS5_STENCIL_EXPORT`.
- **W6**: `night/merge` merged (39 conflicts, both sides kept, list below); the suite is green (736 of 736, after the
  `agc_formatted_buffer_once` rework at the end of the night); the default-on list for the GTA V check is under W6.
- Also: a `spirv-val` sweep over every agc test module (85 invalid -> 3; the two generic fixes are on `night/merge` too),
  specialization output 36% smaller over the test modules (dead blocks, one constant per value, values shared along
  the dominator tree, select / bitcast folds; all also on `night/merge`; **the value sharing first merged loads across
  stores, fixed at the end of the night in `2849a40b`**), and two W5 refusal messages that now carry the address / descriptor words. W3 and W5 need requests from the day
  session.

### Commits

| Commit | Task | Switch | What the test proves |
|---|---|---|---|
| `6f9ef206` | - | none (fix) | the branch builds on Linux again: libkernel's `sceKernelUnknown*` exports were C++ mangled (an `extern "C"` closed early in 3a7e0f9eb), the disk cache's `sizeof(ImageResource)` check was stale (0f72fc824) |
| `ec85e808` | W1 | `APS5_COMPACT_BDA_HELPERS=1` | `agc_driver_bda_device_compact`: the BDA execution tests (faults, their reasons and order, byte-granular ranges) pass with the compact helpers |
| `96aec850` | W1 | `APS5_FORMAT_SHARED_READS=1` | `agc_driver_buffer_format_compact`, `agc_driver_format_d16_compact`, `agc_driver_tbuffer_format_d16_compact`: the runtime-format loads read the same values |
| `2d854575` | W4 | `APS5_STENCIL_EXPORT=1` | `agc_driver_graphics_stencil_export`: the MRTZ stencil component reaches `FragStencilRefEXT` (capability, built-in, store), and the state accepts STENCIL_TEST_VAL_EXPORT_ENABLE only with the switch and a 32_GR / 32_ABGR Z format |
| `e2cf7680` | W6 | none | merge of `night/merge` (upstream main 57f96fd2b + tonight's P commits); the whole suite passes on lavapipe except `agc_formatted_buffer_once`, which failed before the merge too |
| `1916280f` | W1 | `APS5_COMPACT_BDA_HELPERS=1` | `agc_driver_bda_span_reads_compact` (new) and `agc_driver_bda_device_compact`: main's span reads, their faults and partial spans read the same with the looped slow path; the 0x11d049aa00 module passes `spirv-val` with each switch combination (it did not before: the byte loop of `ec85e808` put its condition between `OpLoopMerge` and the branch) |
| `d85f8d3f` | W2 | `APS5_BINDLESS_STORAGE_TABLES=1` | `agc_driver_bindless_storage_images` (+ `_wide` with `APS5_WIDE_BINDLESS=1`): an `image_store` through a bindless table writes the texture of the key the material selects and no other, after write-back to guest memory |
| `e812a833` | W1 | both size switches | `agc_driver_computed_descriptor_formats` (new, + `_compact`): formatted loads through a V# the shader computes (the GPU-descriptor path the format helpers serve) read 32-, 16- and 8-bit components and a selected one the same with and without the switches |
| `fdc27f98` | W1 | none (fixes) | the switch sweep below: no 64-bit type without Int64 from the shared format reads; `agc_shader_memory` counts the compact byte loads |
| `abaec7b7` | - | none (fixes) | `spirv-val` over every agc test module: 85 of 1,517 invalid before, 3 after (equal branch labels from specialization, wide-table `NonUniform` without its capability, shuffle fold of non-vectors); suite unchanged. The two generic fixes are also on `night/merge` (`c36fe0c0`) |
| `20245a58` | W5 | none (messages) | the two W5 refusals name the address / alignment and the V# words (existing tests match the old text as a prefix) |
| `d7c42862` | - | none (test) | `agc_formatted_buffer_once` counts format switches with cases only (folded selections are default-only switches since `abaec7b7`) |
| `a161a164` | - | none (size) | specialization drops dead blocks nothing references: -5.8% words over every agc test module, `spirv-val` and the suite unchanged; also on `night/merge` (`0fdc4484`). 0x11d049aa00: main own 1,861,531 -> 1,851,045 |
| `9154e950` | - | none (size) | specialization reuses one constant per type and value (it made one per folded value): -26% words over every agc test module (7.77M -> 5.73M), 0x11d049aa00 2,067,230 -> 1,957,216 words; `spirv-val` and the suite unchanged; also on `night/merge` (`15bcc38ee`) |
| `2e42e55a` | - | none (test) | `agc_formatted_buffer_once` bounds the growth of two more loads by the shared access they call instead of a tenth of the module (a limit that stopped meaning anything once the modules lost their duplicate constants); the test passes again here and on `night/merge`, **the whole suite is green on this branch (736 of 736)** |
| `cadf2d07` | W1 | none (size) | specialization shares identical pure values within a block: -7.7% more over the test modules (5.73M -> 5.29M words); 0x11d049aa00 1,957,216 -> 1,813,647 words, main own 1.85M -> 1.71M, **14.96M inlined** with both switches; suite 736 of 736, `spirv-val` unchanged; also on `night/merge` (`d69e6a92a`) |
| `e2b94d48` | W1 | none (size) | specialization folds nested selects on one condition and bitcast round trips: 0x11d049aa00 1,813,647 -> 1,792,307 words, main own 1.69M, **14.90M inlined** with both switches; suite 736 of 736, `spirv-val` unchanged; also on `night/merge` (`79ee779e`) |
| `c3ebf7e9` | W1 | none (size) | value sharing along the dominator tree: 0x11d049aa00 1,792,307 -> 1,629,298 words, main own 1.53M, **14.68M inlined** with both switches; test modules 5.27M -> 4.98M; `spirv-val` unchanged; suite 736 of 736 (one full run had `agc_driver_pattern_buffer_fill` fail once under load: it passed 30 of 30 alone and in the next full runs; the change is deterministic, so a wrong merge would fail every time); also on `night/merge` (`6715f88f`) |
| `1efd0f8c` | W1 | `APS5_COMPACT_BDA_HELPERS=1` | a fault-recording lookup breaks out of a default-only switch construct to one shared fault site (same first failing reason) instead of recording at each of its 4 check stages: `get_bda_pointer` inlines 1 fault record instead of 4; 0x11d049aa00 **13.32M inlined** with both switches; the compact BDA (fault reasons and order), span-read and format tests pass; suite 736 of 736; `spirv-val` unchanged |
| `2849a40b` | W1 | none (fix) | **exactness fix for `cadf2d07` / `c3ebf7e9`**: value sharing counted `OpLoad`, `OpPhi` and the derivatives as pure (`PureInstruction` means side-effect free), so two loads of one pointer could merge across a store between them, and derivatives across divergent flow. Never shared now: 0x11d049aa00 13.32M -> 13.39M inlined, test modules 4.98M -> 5.29M. No test caught it; suite 736 of 736; also on `night/merge` (`0baa0273`) |
| `f6d83a57` | W1 | none (size) | loads through a pointer rooted in a UniformConstant, Input, Uniform or PushConstant variable are shared again (read-only within an invocation): 0x11d049aa00 1,631,665 words, main own 1.53M, **13.36M inlined**; test modules 5.24M; `spirv-val` unchanged (3 known errors over 1519 modules); suite 736 of 736. Block-local sharing of the other loads between stores was tried: -0.04%, not kept. Also on `night/merge` (`dcefc032`) |
| `d277c6dc` | W1 | none (size) | `record_bda_fault` no longer stores the fault record's reserved word (zero from the buffer's creation and every reset, nothing else writes it, `CheckFault` requires it zero): 135 -> 126 words at every inlined fault site; 0x11d049aa00 **13.19M inlined** with both switches, `spirv-val` passes; suite 736 of 736 (the BDA fault tests read the whole record). Also on `night/merge` (`f504109a`) |
| `2aca21f3` | W1 | `APS5_COMPACT_BDA_HELPERS=1` | the compact span read's slow dword loop recomputes each dword's address instead of choosing the first dword's (same address; its overflow fault can only repeat the one already recorded, and `record_bda_fault` writes only while none is): `read_bda_span` inlined 2,892 -> 2,819 words, 0x11d049aa00 **13.10M inlined**; `spirv-val` passes; the compact span-read and BDA tests pass; suite 736 of 736 |
| `d3633e16` | W1 | none (size) | a GPU-descriptor formatted access checks the address add of each shared read key for overflow but records one fault site on the OR of the conditions (every key recorded the same descriptor base, 0 bytes and pc, with nothing in between): `load_formatted_gpu_buffer_x4` 12 -> 1 overflow sites, 0x11d049aa00 **12.89M inlined**; `spirv-val` sweep unchanged (3 known errors over 1519 modules); suite 736 of 736 |
| `185eaed1` | W1 | none (size) | lead 6 below: the specialization pass removes a `record_bda_fault` call whose guard is dominated by an earlier guard on the same condition (the function writes only while no fault is recorded, so the later call is a no-op; found by name): main's InvalidDescriptor sites 2,012 -> 689, 0x11d049aa00 **12.72M inlined**; `spirv-val` sweep unchanged; suite 736 of 736; `agc_spirv_optimizer_tests` gains the case (SPIRV-Tools builds only; run by hand here: 3 of 4 calls kept, 4 before). Also on `night/merge` (`54724000`) |

### W1: 0x11d049aa00, SPIR-V size

Measured with `agc_shader_replay --spv` on `testdata/wolverine-requests/shader_11d049aa00.req` (Linux, no SPIRV-Tools
optimizer in this build) and a script that inlines every call the way NVIDIA does (`DontInline` ignored): a function's
inlined size is its own words plus each callee's inlined size per call.

| Step | module words | main own | format helper own (x3 / x4) | format helper inlined | whole shader inlined | main + helpers x calls (README metric) |
|---|---|---|---|---|---|---|
| base (260f6499e) | 1,640,501 | 1,400,794 | 32,929 / 33,991 | 429,677 / 430,739 | **92.3M** | 5.6M |
| `APS5_COMPACT_BDA_HELPERS` (byte loop) | 1,640,387 | 1,400,791 | same | 205k | 39.4M | 5.6M |
| + fault sites per lookup stage | 1,640,003 | 1,400,787 | same | 126k | 24.5M | 5.6M |
| + `APS5_FORMAT_SHARED_READS` | 1,608,082 | 1,400,787 | 21,405 / 21,744 | 50k | **15.0M** | **4.1M** |

What the 92M were: every BDA dword read site is a span probe (`probe_bda_pointer`, ~800 words) plus a slow byte path
(`read_bda_dword_bytes`) that called the full lookup once per byte; each lookup had 16 fault-record call sites (16 x
135 words when inlined). One slow path inlined to 12.7k words, and main has 2,643 of them (plus 24 in each of the 126
format helper calls). The comparison shader 0x11d0376e00 (which NVIDIA compiles) has no BDA reads: 2.76M words, 2.42M
inlined.

Where the 15.0M still are: main's own 1.4M; the 126 format helper calls (6.3M); main's 2,641 BDA read sites at ~2.65k
inlined words each (probe 706 + slow path 1,678 + two address-overflow fault calls), 7.0M. main's own words are mostly
the bookkeeping around those sites (Select 10.5%, Bitcast 8.2%, BitwiseAnd 6.4%, labels, calls, phis). Next steps, by
size:
1. Coalesce main's BDA reads at the IR level: loads from one base at constant offsets probed as one span (one probe and
   one slow path per group instead of per dword); 2,641 sites are mostly single-dword reads.
   Measured after the merge: 2,639 of main's 2,641 span reads read one dword; their address expressions (compared six
   levels deep) are 1,831 distinct, so grouping by base can merge at most ~810 sites (~30%, under ~2.7M of the
   15.4M), and only where no store lies between them.
2. Convert runtime formats once per number format with runtime bit widths instead of once per format (the conversions
   are ~8.7k of the helper's 21.7k words): main + helpers x calls 4.1M -> ~3.3M.
3. The lookup's table-header checks (5 loads and compares) run in every probe and lookup; validating the table once per
   invocation would take ~60 words off each inlined lookup (measured after the merge; smaller than first estimated).
4. (after the merge) main's per-operand f32 denormal flush (`TranslationContext::flushF32Denormal`, `flushTinyProduct`,
   for shaders whose MODE flushes f32 denormals) is roughly a quarter of main's own words. A `DenormFlushToZero 32`
   execution mode on devices with `shaderDenormFlushToZeroFloat32` (and a float-controls independence that allows it
   beside the f16 `DenormPreserve`) would replace it, behind a switch; open questions before that: the ops main leaves
   unflushed (transcendentals are not in `f32DenormalFlushFor`), shaders that change MODE part way, and whether
   NVIDIA flushes every op the mode covers.
5. (after the merge) `record_bda_fault` is 135 words and is inlined at ~26k sites of 0x11d049aa00 (5,795 direct in
   main, ~7 per span read, more in the format helpers): ~3.5M of the 15.4M. Deferring faults would take ~110 words off
   each site: a site writes the first fault of the invocation into private variables (~25 words) and one
   `record_bda_fault` runs at each exit of the shader (returns, kills, the BDA stop paths). Same per-invocation first
   fault; the risk is an exit path that misses the flush (a lost fault record), so it needs every exit enumerated and
   a test per exit kind.

Where the 13.36M were before `d277c6dc` (both switches, `f6d83a57`; the fault record is 9 words smaller since): main's
own 1.53M; 2,641 span reads x 2,892 inlined words (7.64M: own 674, probe 702, the slow dword path 1,246, two overflow
fault sites); the 126 format helper calls (3.27M: 78 x3 at 25.8k, 48 x4 at 26.2k); 5,795 fault sites directly in main
(0.78M: 3,440 address-add Overflow, 2,012 InvalidDescriptor, 343 Unmapped). Two more leads measured:
6. (done in `185eaed1`) `record_bda_fault` writes only while no fault is recorded (compare-exchange on state 0), so a fault site guarded by
   the same condition value as a dominating fault site is a no-op. main's 2,012 InvalidDescriptor sites have only 686
   distinct conditions (one GPU descriptor validated at every access): eliding the 1,326 repeats is ~0.18M. The
   Overflow and Unmapped sites all have distinct conditions. Needs dominance (the specialization pass has it, but
   would have to know the fault function is first-write-wins; the emitter knows that but not dominance).
7. The two overflow sites inside `read_bda_span` record the same arguments (base address, 0 bytes, pc); merging them
   into one would save 126 words x 2,641 = 0.33M, but deferring the first past the probe changes which fault is
   recorded first when the probe also faults, and the compact BDA tests pin that order.

Live check for the day session (`ps5run/captures/wolverine-cutscene`, both switches on with the usual Wolverine set):
the replay reaches `[replay] loop 0` with 0x11d049aa00 not skipped (no `[gpu] skipped` for it), the pipeline compile
time of that dispatch (`[pipeline-cache] saved` timing and the dispatch's first frame), and its frame (the 1024 x 300
rectangle and the stair-stepped terrain edges should be gone if the dispatch runs).

### W4: stencil export

`APS5_STENCIL_EXPORT=1` (`State.cpp`) accepts DB_SHADER_CONTROL STENCIL_TEST_VAL_EXPORT_ENABLE when the Z format is
32_GR or 32_ABGR (the formats whose MRTZ export carries a stencil component). The recompiler always honours
`ShaderPixelStageInfo::stencilExportEnable`: the MRTZ export's component 1 is stored to a `FragStencilRefEXT` output
(capability `StencilExportEXT`, `SPV_EXT_shader_stencil_export`, execution mode `StencilRefReplacingEXT`), the flag is
in the pixel input state, the draw cache key and the request serializer (version 17 after the W6 merge).
`VulkanDevice` enables `VK_EXT_shader_stencil_export` when present. Test: `stencilExportTests` in `tests/Graphics.cpp`
(`agc_driver_graphics_stencil_export` runs the graphics tests with the switch: the module has the capability, the
built-in and the store; the state accepts the bit only with the switch and a stencil-capable Z format).

### W6: night/merge merged into night/wolverine-merge

Merge of `night/merge` (`70a8af4f6`, which carries upstream main 57f96fd2b and tonight's P commits) into this branch: 39
conflicted files, every one resolved by keeping both behaviours; where main's rework replaced the code, ours is re-applied
inside it. Each resolution:

- GuestAllocations.hpp/.cpp: ours (wolverine) Range::sceProtection + GpuAccessible() (3a7e0f9eb era, SCE protection kept per range) vs main #1961 Range::gpu (GPU-mapped at map time). Both: Range carries both; Add/Protect take (readable, writable, gpu, sceProtection = -1); replaceRange keeps both per piece.
- DirectMemory.cpp: ours readable/writable from the native protection and the SCE protection passed on; main's gpu = prot & GPU read/write and the flexible map's alignment (main). Both.
- SpirvBdaRead.cpp: ours BdaHelpersCompact (tonight's ec85e808) + main's span read functions (#1267, b95842597); both; main dropped two unused locals (taken).
- RuntimeAbi.hpp: ours sampler heap 64 + WideImageCapacity (runtime ABI 15) vs night/merge's merge of main (ABI 12: sampled 256, storage 64, samplers 32): the larger of each (samplers 64), ours' ShaderData size; Version 16 (both changed).
- RequestSerializer.cpp: night/merge's numbering (14 packing, 15 dual source, 16 quad mask) + ours stencil export at 17 (was 15 tonight). The two testdata requests are compute (no pixel fields), readable under both.
- SharedMemoryInstructions.cpp (3 ds atomics): ours sharedAddress(inst) + main's flushDenormals (sharedFloatFlush); both.
- InstructionTranslator.cpp (mesh fast launch): main generalized the per-wave fields to options.waveSize, the same as ours' meshWave for waves 32/64; main's kept, ours' wave-size guard (32 or 64 only) re-applied.
- Recompiler.cpp: main's prepared frontend (PreparedControlFlow, AcquireFrontend) replaces ours' inline decode/graph/structurize; ours' APS5_SUBROUTINE_CALLS inlining re-applied inside the frontend's decode, and frontend reuse is off when it applies (the inlined program depends on user data and memory). PrepareShader: main's overload with the preparation context, inside ours' RunWithDeepStack.
- Structurizer.cpp/.hpp: both added a retry around structurizing. Main's (retry with shared-return privatization) runs first; ours (privatizeSharedExits, then structurize) runs if main's retry also throws. structurizeGraph renamed to main's structurize(graph, false).
- tests/Structurizer.cpp: both sides' helpers and tests kept (requireDominatedMerges, makeSizedGraph, shared early-exit fixtures + main's exact post-dominator and dominance-set fixtures).
- Apr.cpp: the wait keeps our deferWaits early return and takes main's PreciseSleepUs(50) in place of sleep_for; submit keeps our _aprSubmit (trace + typeless buffers), which already runs main's _execute path.
- libSceAgcDriver/CMakeLists.txt: both sides' tests kept (depth-clear-draws + shader_mode_register, CPU thread dims + prepared-shaders resource lock); the foreach list is main's, a superset of ours.
- Driver.hpp: main made dumpRequest public; it keeps our suffix parameter (default "").
- ShaderMemory.hpp, SrtWalker.hpp: our block reader (readBlock / SrtBlockReader) and main's accessibility probe (accessible / SrtMemoryProbe) both kept.
- DepthSurface.hpp, GuestBufferMemory.hpp, GuestTextureResource.hpp: both sides' declarations/fields kept (DepthSurfaceExtents + NoteDepthPlaneFill and HtileDepthClearAddress + NoteHtileDepthClear; pinned and chunk; the baseZeroIsNull overload and DescriptorSingleLevel).
- Execution ShaderMemory.cpp: our readBlock and main's accessible both kept and both wired into the runtime.
- VulkanDevice.cpp: our ThreadGroupConverter and main's WorkgroupMemory both kept; PresentDisplayBuffer keeps our APS5_PRESENT_SURFACE substitution and then runs main's APS5_STORE_AT_FLIP flush.
- BufferFill.cpp: main's restructure (dword-pattern and pattern-fill kernels, then the record fill) taken; our zero-kernel fill re-applied inside it (it needs only 4 user-data words and fills zeros instead of reading userData[4..7]).
- Execution Draw.cpp: our fragment input linking and main's StagePushOffset both kept.
- PacketExecution.cpp: our persistent VGT_DMA_MAX_SIZE / VGT_DMA_BASE capture and main's landPendingWaits both run for a draw packet.
- Pm4.cpp: the reset keeps the marker stack (ours) and builds the fresh QueueState on the heap (main's make_unique, avoiding a large stack temporary).
- State.cpp: main now decodes DB_RENDER_CONTROL depth/stencil clear bits natively (ALWAYS compare + clear stencil replace) and that is taken; our APS5_DEPTH_CLEAR_DRAWS block stays after it and, when on, still marks depthClearDraw / stencilClearDraw so the draw records our vkCmdClearAttachments path.
- Graphics Draw.cpp (2 hunks): our GPU checkpoint marker and clear-draw path kept; otherwise main's recordDrawCommands with the split-faces second pass.
- Resources.cpp: main's host-visible slab path taken; the dedicated allocation keeps our memoryTypeBits fallback argument.
- ShaderInputState.cpp: main's nullProgram export mapping plus our stencilExportEnable.
- DepthSurface.cpp: includes unioned.
- SpirvRuntimeBuffer.cpp: ours (the layout-grouped format load switch and APS5_FORMAT_SHARED_READS); main had only the old formatSwitch head.
- Thread.cpp (Windows): main's HOST_STACK_MARGIN taken; our 4 MiB native-stack minimum re-applied as max(stackSize + margin, 4 MiB).
- VideoOutDriver.cpp: main's reworked processFlip (overlay bus, unregistered / black buffers) taken; our APS5_TRACE_FLIPS line re-applied before the present.
- Output.cpp: main's named sceVideoOutAddVrrStatusFlagsPrivilege replaces our sceVideoOutUnknown00 (same NID kP2L8t3j-aM, returns 0).
- NpEntitlementAccess Export.cpp: indentation only, main's.
- Our other unnamed exports that disappeared in the merge (sceVideoOutUnknown01 = LibwuIonIBw, 12 libcCyberUnknown*, 2 sceNpPartnerUnknown*, sceAgcCreateInterpolantMapping's alias) are all named functions on main now with the same NIDs (checked by hashing the names); nothing to re-add.
- tests/Pm4.cpp: main's tolerant unbalanced marker pop taken; the marker is pushed again so our "dispatch reset keeps the open marker" check still runs.
- tests/Graphics.cpp: main's depth/stencil clear decode test replaces our "clears refused without APS5_DEPTH_CLEAR_DRAWS" expectation (main now accepts them); our switch-on block above it stays.
- tests/ShaderMemory.cpp: serializer prefix moved to version 17 (ours: stencil export after main's 16), unsupported probe 18.
- Resources.hpp: ImageResource got main's four constant-swizzle/flat bools and our wideTable; wideTable moved beside the other bools so the struct stays 120 bytes (the disk cache's static_assert; both encoders already write every field).
- libSceSaveData.native Export.cpp: both sides added the same sceSaveDataCancel stub at different places (auto-merged into a redefinition); one kept.
- tests ThreadIndirectDispatch.cpp, DepthClearDraw.cpp (ours): GuestAllocations::Add gained main's gpu flag; our test buffers are GPU-visible (true).
- Found by the suite after the merge: main's new runtime image modes (constant swizzle, flat-volume and flat-line views, which change the dimension) could be chosen as a wide table's per-class mode, and LoadSampledImageDescriptor read wideTable from program.Info().images, which is empty when main's runtime image mode is set (direct Recompile of a request without a capture). WideModes now skips those modes; the descriptor load reads the mode it was given. Fixed agc_shader_memory, agc_shader_bindless*, agc_driver_bindless_images*, agc_driver_image_* and ~60 other image tests.
- tests GdsAppend.cpp / SdwaCompareScan.cpp (ours, from wolverine-capture): no subgroup-size skip, so on lavapipe (8-lane subgroups) the wave32 append and row scan split at lane 8; they now skip like main's subgroup tests (subgroup size < 32).
- LibAtrac9: main's merged source calls the newer LibAtrac9 API; the worktree's submodule files were older (same gitlink); the build tree's copy was refreshed (no repository change).
- Apr.cpp (ours, found by main's guest_ampr_markers): the APR submit queue was a function-local static that exit() destroyed while our detached worker waited on its condition variable, so the process hung at exit; the queue is now never destroyed.

Suite after the merge (lavapipe, `ctest -j4`): every test passes or skips except `agc_formatted_buffer_once`, which
fails on this branch before the merge too: its limit is relative (two more loads add under a tenth of the module);
this branch's shared format function is half the size of night/merge's (6,863 words with one load against 11,404),
while each extra load costs ~428 words here and ~298 there; the call-site code is the same on both, so the extra
~130 words per load come from elsewhere: per added load, main grows ~200 words and the module-level constants
~225 (about 80 `OpConstant` and 40 boolean constants per load site, the folded specialization constants), the
shared function not at all. The test now counts only format switches with cases (`abaec7b7` turns folded selections
into default-only `OpSwitch`es, which it counted); its word limit still fails as above. The first run
found the three problems above (wide tables against main's runtime image modes; the two capture tests without a
subgroup-size skip; the APR queue's teardown hang); all are fixed in the merge commit.

Re-measured 0x11d049aa00 after the merge. Main's #1267 replaces the per-dword BDA reads with `read_bda_span` (one probe
per span of up to four dwords, then a byte path per used dword when the span is unmapped or unaligned); our two size
switches still apply to its pieces, but the span function inlines each dword's slow path separately:

| Build | module words | main own | `read_bda_span` inlined x 2,641 calls | format helper own (x3 / x4) | whole shader inlined | main + helpers x calls (README metric) |
|---|---|---|---|---|---|---|
| before the merge, both switches | 1,608,082 | 1,400,787 | (per-dword sites, 2.65k each) | 21,405 / 21,744 | 15.0M | 4.1M |
| after the merge, no switches | 2,112,644 | 1,861,531 | 53.6k | 31,418 / 32,480 | 319.3M | 6.3M |
| after the merge, both switches | 2,082,654 | 1,861,523 | 9.2k | 21,153 / 21,492 | **36.9M** | 4.5M |

So the merge took the inlined size from 15.0M to 36.9M with the switches (24.3M of it in the span reads). The next
commit loops the span's dword slow paths under `APS5_COMPACT_BDA_HELPERS` (see "W1 after the merge" below).

One replay of the request right after the build failed with `wide bindless table has no WideImages binding or element`
and later runs did not; it is the image-mode bug fixed above (the binary of that run predates the fix), noted in case it
shows up live.

#### Default-on changes of this branch that GTA V has not been checked with yet (day session)

These run for every game on `night/wolverine-merge` without a switch. None has been run on GTA V (the replay
`gta-perf-3` plus one live run is the check before the fold into `night/merge`):

| Change | Where | What to look for on GTA V |
|---|---|---|
| APR hybrid execution (deferred waits, `_aprSubmit`) | `libkernel/Apr/src/Apr.cpp` | streaming stalls or missing assets at load |
| Structurizer relaxed join and the shared-exit privatization fallback (now after main's shared-return retry) | `ControlFlow/src/Structurizer.cpp` | shader refusals that were not there (`[gpu] skipped`), different images |
| DeepStack (host code on guest threads, 4 MiB native stack minimum on Windows) | `Thread.cpp`, `Recompiler.cpp` | none expected; memory per thread |
| Heap mirrors in ReBAR device memory (`APS5_HEAP_MIRROR_HOST=1` restores the old) | `GuestBufferMemory.cpp` | frame time, VRAM use |
| Whole-range pinned imports | `GuestBufferMemory.hpp` (`pinned`) | import counts on the `[buffers]` line, RAM |
| Draw snapshot cap (`APS5_DRAW_SNAPSHOT_MAX_MIB`, default 256) | `ShaderResources.cpp` | draws refused for a too-large snapshot |
| Aperture registers | recompiler | none expected |
| Fast-launch point-per-subgroup (wave-size guard) | `InstructionTranslator.cpp` | mesh draws |
| GPU thread-group conversion for indirect dispatches (`APS5_CPU_THREAD_DIMENSIONS=1` = CPU) | `VulkanDevice.cpp` | indirect dispatch results |
| Runtime ABI 16 binding numbers (wide images 63.., feedback 84) | `RuntimeAbi.hpp` | pipeline layout errors on the validation layer |
| Pixel inputs read their default when the previous stage does not export them | `Execution/src/Driver/Draw/Draw.cpp` | none expected |
| The GDS M0 base | recompiler | GDS users |
| Device heap fallback (dedicated allocations retry over the other memory types) | `Resources.cpp` | none expected |
| Clearing a replaced device's textures (`APS5_KEEP_REPLACED_TEXTURES=1` keeps them) | `VulkanDevice.cpp` | device recreation |
| Wide table memo and class memo | `ResourceMaterializer.cpp` | none (only with `APS5_WIDE_BINDLESS`) |
| The zero-fill kernel HLE with 4 user-data words | `BufferFill.cpp` | `[fill]` counts |
| Persistent VGT_DMA_MAX_SIZE / VGT_DMA_BASE from DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2 | `PacketExecution.cpp` | indirect indexed draws |
| A PM4 queue reset keeps the open markers | `Pm4.cpp` | none (markers are diagnostics) |

### W1 after the merge

`read_bda_span` (main, #1267) falls back, for a span the probe does not map as one aligned range, to one byte-path call
per used dword: four inlined copies of the dword slow path (4 x 1,678 words with the compact helpers). Under
`APS5_COMPACT_BDA_HELPERS` that fallback is now a loop over the used dwords (same order, same per-dword address
overflow faults, the first dword reusing the probed address), and the span's address checks share their fault sites.
The byte loop inside `read_bda_dword_bytes` (tonight's `ec85e808`) computed its condition between `OpLoopMerge` and
`OpBranchConditional`, which `spirv-val` rejects (lavapipe accepted it); fixed in the same commit.

| Build (0x11d049aa00, after the merge) | module words | `read_bda_span` own / inlined | whole shader inlined | spirv-val |
|---|---|---|---|---|
| no switches | 2,112,664 | 1,113 / 53.6k | 319.3M | passes |
| `APS5_COMPACT_BDA_HELPERS` only | 2,111,644 | 683 / 3.3k | - | passes |
| both switches, before this commit | 2,082,654 | 1,113 / 9.2k | 36.9M | fails (byte loop) |
| both switches | 2,082,223 | 683 / 3.3k | **18.4M** | passes |

Of the 18.4M: main's own 1.86M, the 126 format helper calls 6.8M (54k inlined each: 21.5k own plus their span reads),
main's 2,641 span reads 8.8M, main's 5,795 direct fault records 0.8M. The README metric (main + helpers x calls) is
4.5M; the goal of 1.5M needs the next steps listed under W1 (coalescing main's reads per base, one conversion per
number format), now with main's span reads as the unit: 2,641 spans are still mostly one dword each.

Second step after the merge (both switches): a format helper read each of its component positions (keys) with its own
inlined read, 4 span reads and 12 byte lookups per helper call. The keys are now read in one loop, in the same order and
with the same per-key reads (a key's width picks a span, 16-bit or 8-bit read in the loop body), so the body inlines one
read of each width. 0x11d049aa00: 18.4M -> **15.4M** inlined words (helper call 54k -> 30k), `spirv-val` passes.

Correction to the W1 table above: the `_compact` ctests named there (`agc_driver_buffer_format_compact` and the d16 ones)
never reached the GPU-descriptor format path (`formattedGpuCall`), so `96aec850`'s shared reads were checked on that path
only by `spirv-val` and the replay until now. `agc_driver_computed_descriptor_formats` (V# word 3 ORed with a
`v_readfirstlane` result, so the descriptor is not a static source) covers it, plain and with both switches.

Whole-suite check of tonight's switches (lavapipe): every `agc_*` ctest run with `APS5_COMPACT_BDA_HELPERS=1
APS5_FORMAT_SHARED_READS=1 APS5_BINDLESS_STORAGE_TABLES=1 APS5_STENCIL_EXPORT=1` in the environment, every compiled
module written out and run through `spirv-val --target-env vulkan1.3 --scalar-block-layout`. Results with the
switches and without them are now the same: 357 of 358 tests pass (`agc_formatted_buffer_once` fails either way),
1,432 of 1,517 modules validate, and the 85 that do not have the same errors without the switches (36 `NonUniform`
decorations without `ShaderNonUniform`, 44 `OpBranchConditional` with equal labels under SPIR-V 1.6, 2 image `Offset`
outside gathers, 2 `OpVectorShuffle` of a non-vector, 1 `CullPrimitiveEXT` type: existing emitter issues on the merged
branch; `abaec7b7` fixed all but the 2 offsets and the `CullPrimitiveEXT` array, which the validator wants as a scalar). The check found two things in tonight's switches, fixed: `APS5_FORMAT_SHARED_READS`
declared a 64-bit type in shaders without the Int64 capability (non-GPU descriptors), and `agc_shader_memory`'s count of
byte loads assumed the unrolled byte reads.

### W2: bindless storage image tables

What the refusal was: `image_load` through a table already works (it is a sampled-class op read with `OpImageFetch`), so
a storage table is one the shader writes or does atomics through. The tracker refused those because every bound slot of
the table becomes a pending write-back, a cost question rather than missing machinery: the non-wide table path already
binds a table's entries as heap slots (root plus `APS5_BINDLESS_SLOTS - 1`, default 16, mode M from the material
records or mode T for a whole small table), `TableImageIndex` already indexed storage heaps, and the driver binds storage
slots like any storage image (write-back included, null slots skipped).

`APS5_BINDLESS_STORAGE_TABLES=1` (Fix, off by default):
- `ResourceTracker` and the materializer accept storage tables; a storage table always takes the slot path, also under
  `APS5_WIDE_BINDLESS` (`ResourceMaterializer::WideTableImage`), so its entries go to the storage heap (64 elements:
  16 slots of a dynamic-mip image fill it).
- The image emitter's `image_store` and image atomics index the storage heap with the table slot (non-uniform when the
  slot is), and skip the access for a key the table does not map (atomics return 0), as reads return zeros.

Not covered yet: a storage table wider than the slots (refused as for sampled tables: `bindless image table has N
entries`), and a WideImages-style storage binding for large tables. Whether 0x11d0395500 fits in 16 slots is unknown
offline (no request in the repository); the live check below tells.

### Not done tonight

- **W5 (small refusals)**: the two refusal messages now carry what the next look needs: `null or misaligned address
  0x... (alignment N, M bytes)` (it was the bare text, from `GuestMemory::CheckRange`, which many callers share) and
  `buffer descriptor uses an unsupported type (w0 w1 w2 w3)`. Note on the second: `addGuestBuffer` checks the V# type
  before its empty check (address 0 or size 0 binds an empty buffer), so a slot holding an image descriptor or a
  zeroed-but-typed V# is refused even when it is never read; if the live words show that, an empty binding for such a
  V# (behind a switch) is the likely fix.
- **W3 (readlane after a wave64 OR-scan)** and the rest of **W5**: no request or log of 0x11d00c4c00, the eboot
  compute 0x7ff6af15db00 or the DRAW_INDEX_2 draw into 0x3c67b0000 is in the repository, so neither could be started
  offline. Day session: save the requests (`APS5_DUMP_SHADERS=1`) and the refusal lines.

### Live checks (day session, RTX 5080, `ps5run/captures/wolverine-cutscene`)

1. W1: with `APS5_COMPACT_BDA_HELPERS=1 APS5_FORMAT_SHARED_READS=1` and the usual Wolverine set, the replay reaches
   `[replay] loop 0` with 0x11d049aa00 not skipped; note its pipeline compile time and whether the 1024 x 300 rectangle
   and the stair-stepped terrain edges are gone.
2. W2: with `APS5_BINDLESS_STORAGE_TABLES=1`, 0x11d0395500 is no longer refused with `bindless storage image tables are
   unsupported`; if it is refused for capacity instead, note the entry count (`[bindless]` line with
   `APS5_TRACE_BINDLESS=1`).
3. W4: with `APS5_STENCIL_EXPORT=1`, the draw into 0x113f230000 is no longer refused (`DB_SHADER_CONTROL`), and the
   frame does not change elsewhere.
4. W6: the merged branch runs the cutscene as before the merge (same frame, fps); the default-on list above on GTA V.
