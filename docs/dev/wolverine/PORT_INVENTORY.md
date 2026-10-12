# Wolverine port inventory: wolverine-capture onto night/wolverine-merge

Every commit `wolverine-capture` (b6f9f526d) has under `core/` that night/merge (f1e595f39^1) did not, with what
became of it on `night/wolverine-merge`. Coverage is the share of the commit's added lines (25+ characters) that
are in the branch now; a superseded commit was replaced by the #639 design or by an upstream change and needs
nothing; "not needed" says why with the evidence; "re-implement" is a pre-#639 feature written again on the #639
design (the old code is not portable). Nothing is left to port: the branch is kept as the tag
`archive/wolverine-capture`, the reference for the re-implement rows.

Totals: 21 diagnostic, 14 not needed, 94 ported, 5 re-implement, 83 superseded, 3 tool/test.

## Open bugs found while porting

- `GdsAppend` (old branch test, from c6e5529a0): the GDS counters are right, the per-lane ds_append results are not
  stored (0xdeadbeef), with or without the M0 port. The test is kept out of the suite until it passes.
- `SdwaCompareScan` (old branch test, from c6e5529a0): the same symptom, the lane outputs stay 0xdeadbeef (also
  with the output buffer in the request memory regions). Both tests dispatch through `device.Dispatch(result, x, y,
  z, {}, codeAddress)`; find why their stores are not written back, then fix the driver or the tests. Both files
  are in the archive tag.
- Whole files only the old branch has: game-shipped module replacements (libcohtml, libfmod, libfmodstudio,
  libREDGalaxy64, libRenoirCore.PS5, libSceNpCppWebApi) that upstream dropped on purpose (79057e916);
  libSceNgs2, libSceAvPlayer.native and libSceAudiodec.native, built from native sources upstream; VarArgsAbi.hpp,
  removed upstream (faad75d65); ShaderPreparation (pre-#639, superseded); and the tests of the rows above.

## Fixed: the GPU device loss at the menu load and the settings (9978fb57a)

A spin-wait in eboot compute 0x7ff71095cc00 (decoupled look-back) never ended. Cause: compute shaders with a dispatch
thread limit prepared at first use failed their own invocation match (push data rule), so their first dispatch was
skipped; among them kernels of the same scan, whose unset state made the look-back wait forever. Found with
APS5_GPU_CHECKPOINTS, APS5_WATCH_MEMORY + APS5_WATCH_PRINT and the capture ps5run/captures/wolverine-merge-905;
APS5_LOOP_GUARD is no longer needed.

## Commits

| Commit | Date | Status | Subject | Note |
|---|---|---|---|---|
| `b77d5c585` | 2026-10-01 | ported | fix(agc): null sampled descriptors bind a 1x1 texture that honours DST_SEL | 60% of its lines are in the branch |
| `5aabf4594` | 2026-09-29 | ported | fix(shader): packed pixel ancillary as SampleId and Layer, gl_Layer via Geometry, Z export formats | 71% of its lines are in the branch |
| `24b24a538` | 2026-10-01 | ported | feat(agc): host import windows, least-recent eviction under the budget, recoverable GPU out-of-memory | 89% of its lines are in the branch |
| `7dcbc6556` | 2026-10-01 | ported | perf(agc): draw cache keys on the user words a program reads, memoizes failures, serves data-only hits | 67% of its lines are in the branch |
| `e558b8e05` | 2026-10-02 | ported | fix(agc): host imports never overlap | 94% of its lines are in the branch |
| `3b63acd94` | 2026-10-02 | ported | feat(agc): depth bounds test and depth bias | 96% of its lines are in the branch |
| `2ed6cec32` | 2026-10-02 | ported | feat(agc): 4 KiB standard color targets and color views of one array slice | 75% of its lines are in the branch |
| `acb176a05` | 2026-10-02 | ported | fix(agc): depth fast clears through the HTILE metadata | 62% of its lines are in the branch |
| `a8f91ac06` | 2026-10-02 | ported | feat(graphics): mipmapped 3D textures | 87% of its lines are in the branch |
| `9726b6a94` | 2026-10-02 | ported | fix(agc): fragment SampleId, Layer and FragDepth interfaces | 82% of its lines are in the branch |
| `dd41c94ff` | 2026-10-02 | ported | fix(agc): accept a fragment SampleMask output | 94% of its lines are in the branch |
| `d3e1f345a` | 2026-10-03 | ported | fix(agc): address-based builds refuse imports past the budget instead of evicting; default budget from RAM | 90% of its lines are in the branch |
| `24e6e76df` | 2026-10-03 | ported | fix(agc): explicit return type for the import budget lambda | too small to measure; merge fix-up or one-liner |
| `809b21cec` | 2026-10-02 | ported | fix(agc): depth fast clears through the HTILE metadata | 62% of its lines are in the branch |
| `9274a09ed` | 2026-10-03 | ported | feat(agc): multisampled color and depth targets with CB resolves | 98% of its lines are in the branch |
| `213364957` | 2026-10-02 | ported | fix(agc): fragment SampleId, Layer and FragDepth interfaces | 82% of its lines are in the branch |
| `cea607922` | 2026-10-02 | ported | fix(agc): accept a fragment SampleMask output | 94% of its lines are in the branch |
| `09b5508ed` | 2026-10-02 | ported | feat(agc): vertex clip and cull distances | 93% of its lines are in the branch |
| `fd0e24b9f` | 2026-10-03 | ported | merge: color swizzle tables for both tile modes (#377 + #404) | 52% of its lines are in the branch |
| `98134ee4e` | 2026-10-03 | ported | merge: buffer pool trim over keyed free slots (#290 + #401) | 100% of its lines are in the branch |
| `d59aeebaf` | 2026-10-03 | ported | merge: close multisampleTests after the clip distance tests (#419 + #423) | too small to measure; merge fix-up or one-liner |
| `1690c883e` | 2026-10-03 | ported | feat(agc): depth surfaces read as textures and storage images | 90% of its lines are in the branch |
| `d4307500b` | 2026-10-03 | ported | merge: multisampled color targets refuse array slices (#377 + #419) | too small to measure; merge fix-up or one-liner |
| `ef7840a63` | 2026-10-03 | ported | merge: CB resolves into 4 KiB standard targets (#377 + #419) | too small to measure; merge fix-up or one-liner |
| `01c11389d` | 2026-10-03 | ported | fix(agc): refuse depth bounds outside [0, 1] without VK_EXT_depth_range_unrestricted | 100% of its lines are in the branch |
| `2ecab6746` | 2026-10-03 | superseded | merge: clip distances at request serialization version 7 (#303 + #423) | upstream clip distances (09b5508ed at 93%) |
| `4c72d427b` | 2026-10-03 | ported | feat(agc): single-sample sample masks as a per-quad-pixel discard | 79% of its lines are in the branch |
| `b14c340fa` | 2026-10-03 | ported | feat(agc): render into one depth slice of a 3D color target (#449) | 91% of its lines are in the branch |
| `af77bac53` | 2026-09-30 | superseded | wip Wolverine boot: AMM, fixed reservations, TLS for guest modules, PngEnc, SyncOnAddress | upstream AMM / SyncOnAddress / PngEnc |
| `9d4ba5a8d` | 2026-09-30 | superseded | wip Wolverine: runtime loads reuse guest modules, hinted mappings, GDS, duplicate table image handles | upstream module loader / GDS |
| `c3179b5cc` | 2026-10-03 | superseded | wip Wolverine: drop what main now has (SyncOnAddress, PngEnc types, coredump stubs), GDS in driver memory | cleanup of the old base |
| `0e0bd507b` | 2026-10-03 | superseded | wip Wolverine: exports the #144 base carried (Psml, AMM, SyncOnAddressWait32/64, posix), mask the internal fle | upstream exports (no unresolved NIDs at runtime) |
| `5eb7c13dc` | 2026-10-03 | superseded | feat(recompiler): bind large bindless image tables as descriptor arrays | wide tables |
| `a76533c9b` | 2026-10-03 | superseded | wip Wolverine: the internal flexible-map flag maps | upstream guest memory test |
| `c21eee4b3` | 2026-10-03 | ported | fix(driver): bound transient texture upload copies within one draw or dispatch | 72% of its lines are in the branch |
| `c6e7729df` | 2026-10-03 | ported | fix(agc): keep the marker stack across queue resets | 100% of its lines are in the branch |
| `84029bf3a` | 2026-10-03 | not needed | feat(agc): share one snapshot image between descriptors of one surface | VRAM: as a49f67a44 |
| `71b543c6f` | 2026-10-03 | diagnostic | feat(agc): APS5_TRACE_TEXTURE_MEMORY reports host image bytes per guest address | port on demand |
| `bc8fb2a09` | 2026-10-03 | superseded | fix(agc): bound the images cached resources pin, evict aliased extents of a block | #639 resource cache |
| `485148096` | 2026-10-03 | superseded | fix(agc): out-of-memory relief frees least-recently-used first and gives up on a thrashing packet | #639 out-of-memory handling |
| `edce544ce` | 2026-10-03 | superseded | fix(agc): bind a placeholder for table entries that alias a block the GPU wrote at another layout | wide tables |
| `7bbd63e83` | 2026-10-03 | not needed | feat(agc): sampled lookups view another format's storage image of the same surface | VRAM: as a49f67a44 |
| `33f4115df` | 2026-10-03 | superseded | fix(agc): one layout per block in a bindless table, texture cache budgets from device memory | wide tables |
| `9c8554526` | 2026-10-03 | superseded | fix(agc): accept PA_SC_MODE_CNTL_1 multi-shader-engine primitive discard | upstream accepts the bit (ScanControlMask) |
| `8b6e00b71` | 2026-10-03 | superseded | fix(agc): a depth surface keeps its block's layout in a bindless table | wide tables |
| `c679b3132` | 2026-10-03 | ported | wip: name both sides of a graphics interface mismatch | too small to measure; merge fix-up or one-liner |
| `3480c573a` | 2026-10-03 | ported | fix(agc): pixel inputs whose parameter the previous stage does not export read the default | ported as 6d1a8338a (pixel inputs read their default) |
| `03556d7db` | 2026-10-03 | superseded | perf(agc): validation capture finds lookup records by hash, keeps a table build's records | #639 validation |
| `d071ad6d2` | 2026-10-03 | diagnostic | wip: APS5_TRACE_FLIPS samples the flipped index and buffer | port on demand |
| `ac7eeb0ac` | 2026-10-03 | superseded | fix(agc): one storage image per surface key while an evicted one is still referenced | #639 storage images |
| `8d0a3ade8` | 2026-10-03 | diagnostic | feat(agc): APS5_WATCH_SURFACE traces the writers and readers of chosen guest addresses | port on demand |
| `f8c72cbd3` | 2026-10-03 | superseded | fix(agc): a bindless table keeps each block's last GPU-written layout | wide tables |
| `0d25898bb` | 2026-10-03 | superseded | fix(recompiler): a bindless table binds the shape group of its access, not of its first entry | wide tables |
| `472eef029` | 2026-10-03 | superseded | fix(agc): a bindless table access binds every entry its dimension can read | wide tables |
| `c6e5529a0` | 2026-10-03 | ported | fix(recompiler): DS instructions on GDS add the M0 base | ported as 351f4c5a7 (GDS M0 base); its GdsAppend test fails on the per-lane ds_append results with or without the port: open bug, see below |
| `593ae356c` | 2026-10-03 | ported | feat(agc): watch lines name how a sampled image is served | too small to measure; merge fix-up or one-liner |
| `261bf53db` | 2026-10-03 | superseded | feat(recompiler): bindless tables bind a second array for entries of the other numeric class | wide tables (3 classes) |
| `2786f27a8` | 2026-10-03 | superseded | wip Wolverine: APR submit accepts game-built buffers, versioned AMPR commands, 7.1.4 ports, reserve over mappe | upstream APR (title-built buffers via 2965edc75), AMPR _04_00, 12-channel AudioOut2 |
| `65fef5754` | 2026-10-03 | superseded | wip Wolverine: APR runs on its own ordered queue with real WaitOnAddress_04_00 token waits, AJM resample param | upstream APR + 10-10 hybrid APR |
| `c9f1023a0` | 2026-10-03 | diagnostic | wip Wolverine: null-page shader memory reads as zero (guarded loads), fiber contexts stay open to host writes, | port on demand |
| `9c942986b` | 2026-10-03 | superseded | fix(agc): address-based builds import or mirror large heap ranges in 256 MiB chunks | #639 GuestBufferMemory |
| `5f79b2f98` | 2026-10-03 | ported | wip Wolverine: shader memory test accepts null nested pointers read as zero | too small to measure; merge fix-up or one-liner |
| `379f8cc55` | 2026-10-03 | ported | wip Wolverine: AJM DecodeSplit runs as a split decode job | too small to measure; merge fix-up or one-liner |
| `f33289e8f` | 2026-10-03 | superseded | fix(agc): host import and heap mirror budgets from the host-visible heap, LRU heap chunks | #639 GuestBufferMemory |
| `837508bcf` | 2026-10-03 | superseded | wip Wolverine: AJM GetResampleInfo reports the stored ratio | upstream AJM |
| `1c00598f4` | 2026-10-03 | superseded | fix(agc): address-based builds import heap ranges in the descriptor path's windows | #639 GuestBufferMemory |
| `1ee268b2a` | 2026-10-03 | superseded | wip Wolverine: AJM Opus (codec 24) instances initialize and output silence (TODO decode) | upstream AJM (FFmpeg Opus) |
| `fcdb722fc` | 2026-10-03 | ported | fix(libc): read-write aliases of shared memory spanning several section runs | 63% of its lines are in the branch |
| `03bdcbe8f` | 2026-10-03 | ported | fix(agc): log only heap mirror chunks of 1 MiB or more | too small to measure; merge fix-up or one-liner |
| `619a220e2` | 2026-10-03 | superseded | fix(agc): pack small mirrors into shared slabs, import runs of small ranges as one allocation, size host budge | #639 GuestBufferMemory |
| `11bb8a180` | 2026-10-03 | superseded | fix(agc): address-based builds map only GPU-accessible guest ranges | #639 GuestBufferMemory (protection is recorded; CPU-only ranges are not skipped yet) |
| `5b5910329` | 2026-10-03 | superseded | fix(agc): address-based builds keep their imports and the texture cache keeps a build's entries | #639 GuestBufferMemory |
| `92bd45064` | 2026-10-03 | ported | perf(agc): texture cache budgets default to half of device-local memory | too small to measure; merge fix-up or one-liner |
| `6a743496e` | 2026-10-03 | ported | fix(agc): indirect register packets accept a null placeholder address to patch later | too small to measure; merge fix-up or one-liner |
| `dd1862a7a` | 2026-10-03 | ported | fix(videoout): buffer attributes may change while a flip is pending; queued flips keep theirs | too small to measure; merge fix-up or one-liner |
| `c76c40726` | 2026-10-03 | superseded | wip Wolverine: APR waits on a null address (no-dependency token) are satisfied | upstream AMPR refuses a null wait address at append |
| `29d30b102` | 2026-10-03 | superseded | perf(agc): cheaper dispatch validation and resource reuse; incremental large bindless tables | #639 prepared shaders and recipes |
| `bf5c57669` | 2026-10-03 | ported | perf(agc): thread-dimension indirect dispatches convert to group counts on the GPU; AMPR buffers start with a  | ported 10-10 (thread-dimension indirect dispatches on the GPU) |
| `d5aab2b57` | 2026-10-03 | ported | fix(agc): take the zero form of the buffer fill kernel through the fill HLE | 100% of its lines are in the branch |
| `7439348f3` | 2026-10-04 | ported | fix(agc): run a wait-free submission of another queue after queue 0's earlier submissions | 96% of its lines are in the branch |
| `909d81a70` | 2026-10-03 | ported | fix(agc): record large memory-to-memory DMA_DATA copies on the GPU in queue order | 100% of its lines are in the branch |
| `349eab43e` | 2026-10-03 | ported | perf(agc): read DCC keys behind the driver's own pending key store without a wait | 100% of its lines are in the branch |
| `84b159489` | 2026-10-03 | ported | fix(agc): keep a surface's results in the 64 KiB block it shares with its neighbour | 98% of its lines are in the branch |
| `f1856890c` | 2026-10-03 | ported | fix(agc): consume the write-watch faults of the driver's own CPU stores | 100% of its lines are in the branch |
| `e62a551fa` | 2026-10-04 | ported | perf(agc): answer verifications of the caller's own live stack without a page query | 100% of its lines are in the branch |
| `862decccf` | 2026-10-03 | ported | fix(agc): complete a flip only once the frame's GPU work has finished | 100% of its lines are in the branch |
| `e11cc087f` | 2026-10-03 | ported | perf(shader): build a shader's front end once on first use | 97% of its lines are in the branch |
| `7f98137e4` | 2026-10-01 | ported | perf(agc): copy the interleaved attributes of a vertex buffer once per draw | 100% of its lines are in the branch |
| `5f40cc616` | 2026-10-03 | superseded | perf(agc): build compute shaders' front end when they are registered | #639 registration (front end at registration) |
| `fbb930ed6` | 2026-10-03 | ported | feat(ajm): decode Opus (codec 24) through FFmpeg | 93% of its lines are in the branch |
| `412f5bd69` | 2026-10-03 | ported | fix(agc): draw without a pixel program when SPI_SHADER_PGM_PS is unset | 69% of its lines are in the branch |
| `20c34aa1d` | 2026-10-03 | ported | fix(agc): place device-local allocations in another allowed heap when the device heap runs out | ported as e8af7873c (device heap fallback) |
| `6395472cc` | 2026-10-03 | superseded | perf(agc): replay thread-dimension indirect dispatches through recipes; phase lines for other queues | #639 recipes |
| `114fe4efb` | 2026-10-03 | diagnostic | chore(agc): profile counters for stale texture replacements and recipe build refusals | port on demand |
| `6368ef399` | 2026-10-04 | tool/test | test(agc): agc_dispatch_bench, a synthetic benchmark of bindless, address-based compute dispatches |  |
| `4036b70b0` | 2026-10-03 | ported | perf(agc): build check failure messages only when a check fails | 100% of its lines are in the branch |
| `747fdb94e` | 2026-10-04 | superseded | perf(agc): analyse a bindless table's words once, memoize descriptor sizes for presync, build the mip-range me | wide tables |
| `0f259798b` | 2026-10-04 | ported | feat(agc): frame capture (APS5_CAPTURE) and agc_frame_replay | frame capture and replay are in (0f259798b at 90%) |
| `04bd1ba74` | 2026-10-04 | superseded | perf(agc): judge a texture's guest mappings by the changes reported over its own range | #639 mapping generations |
| `6dc8170aa` | 2026-10-04 | ported | fix(kernel): export DirectMemoryMappings with C linkage so NID patching keeps import and export in step | too small to measure; merge fix-up or one-liner |
| `508879b3d` | 2026-10-04 | ported | fix(agc): frame replay reads event arrays in order, --shader-cache, exits without static teardown | frame capture and replay are in (0f259798b at 90%) |
| `c2aa2825f` | 2026-10-04 | superseded | perf(agc): capture collects only writable GPU-visible pieces; clearer shader count | #639 capture |
| `342590e31` | 2026-10-04 | superseded | fix(ampr): refuse a wait on a null address when it is appended, naming the caller | upstream AMPR refuses a null wait address at append |
| `bf2d78f64` | 2026-10-04 | ported | fix: resolve merge of wolverine-next into frame-capture (both tool targets) | too small to measure; merge fix-up or one-liner |
| `4885945db` | 2026-10-04 | superseded | perf(agc): recipes rebind moved read-only buffers; collect memo, committed collects and draw snapshots judge m | #639 recipes and collect memo |
| `cbc91b507` | 2026-10-04 | ported | fix(agc): capture registered shaders from the driver's own copies; replay inserts them without guest memory | 81% of its lines are in the branch |
| `8caf7463f` | 2026-10-04 | ported | fix(agc): replay registers ranges outside the arena as image ranges (GuestAllocationsAddImage) so address-base | 93% of its lines are in the branch |
| `65e14204d` | 2026-10-04 | ported | fix(agc): replay page-caches the game's main image span (GuestMemory::SetImageRange) as the game does | 100% of its lines are in the branch |
| `dd885c64e` | 2026-10-04 | ported | docs(agc): frame capture and replay usage, format and limitations; [frame-capture] log prefix | frame capture and replay are in (0f259798b at 90%) |
| `5bd7a4e10` | 2026-10-04 | ported | feat(agc): agc_frame_replay --cold clears the driver's caches between loops; replay waits for its PNGs | frame capture and replay are in (0f259798b at 90%) |
| `bab6b546b` | 2026-10-04 | ported | feat(agc): replay --cold=live preset (default), --for-seconds, mapping changes per loop; docs: benchmarking nu | 93% of its lines are in the branch |
| `42a9cdbe6` | 2026-10-04 | tool/test | fix(agc): agc_frame_replay opts out of power throttling; docs: how to measure a driver change | frame capture and replay are in (0f259798b at 90%) |
| `f738738d3` | 2026-10-04 | ported | fix(agc): agc_frame_replay loads at its fixed base | frame capture and replay are in (0f259798b at 90%) |
| `c892b4d0e` | 2026-10-04 | superseded | perf(agc): import large ranges in 1 GiB cells; reuse builds with misaligned GPU copies | #639 import windows |
| `cb9c1434e` | 2026-10-04 | superseded | perf(agc): bindless tables skip duplicate elements in stage A and the table records | wide tables |
| `f6277c2b5` | 2026-10-04 | superseded | perf(agc): import large ranges in 1 GiB cells; reuse builds with misaligned GPU copies | #639 import windows |
| `234dd86c7` | 2026-10-04 | superseded | perf(agc): bindless tables skip duplicate elements in stage A and the table records | wide tables |
| `10a1863a2` | 2026-10-04 | superseded | perf(agc): cached table builds collect and pre-sync each distinct surface once | wide tables |
| `c24380759` | 2026-10-04 | superseded | perf(agc): committed collects answer from this epoch's evidence without the tracker lock | #639 collects |
| `766e7d493` | 2026-10-04 | superseded | perf(agc): reused table builds collect merged spans of their surfaces | wide tables |
| `0b561dfa4` | 2026-10-04 | superseded | perf(shader): read a whole bindless table in one block read | block reader (readMemoryBlock) |
| `041e89ddc` | 2026-10-04 | superseded | perf(agc): table objects revalidate without validation records or repeated collects | wide tables |
| `2606a29b8` | 2026-10-04 | not needed | perf(agc): the GDS buffer gets a host import; DMA_DATA GDS fills are ordered stores | with 639c50f40 |
| `39a1878bd` | 2026-10-04 | superseded | perf(agc): content keys carry a digest of a bindless table's words | wide tables |
| `c82ec3300` | 2026-10-04 | superseded | perf(shader): hash a large descriptor list in four lanes for the result memo | #639 result memo |
| `1b56d79e3` | 2026-10-04 | superseded | perf(agc): the table cache keeps a binding's tables by their words | wide tables |
| `f1e0aab99` | 2026-10-04 | superseded | perf(shader): captures share a memoized table resolution instead of copying it | wide memo (resolveTableImage) |
| `8e7e77734` | 2026-10-04 | superseded | perf(agc): cached table builds collect and pre-sync each distinct surface once | wide tables |
| `4f0275d2b` | 2026-10-04 | superseded | perf(agc): committed collects answer from this epoch's evidence without the tracker lock | #639 collects |
| `539f643fa` | 2026-10-04 | superseded | perf(agc): reused table builds collect merged spans of their surfaces | wide tables |
| `6f0b1be41` | 2026-10-04 | superseded | perf(shader): read a whole bindless table in one block read | block reader (readMemoryBlock) |
| `f096514b0` | 2026-10-04 | superseded | perf(agc): table objects revalidate without validation records or repeated collects | wide tables |
| `91d3657a3` | 2026-10-04 | not needed | perf(agc): the GDS buffer gets a host import; DMA_DATA GDS fills are ordered stores | duplicate of 2606a29b8 |
| `1ab20ed48` | 2026-10-04 | superseded | perf(agc): content keys carry a digest of a bindless table's words | wide tables |
| `60c2356e5` | 2026-10-04 | superseded | perf(shader): hash a large descriptor list in four lanes for the result memo | #639 result memo |
| `fa80d96ab` | 2026-10-04 | superseded | perf(agc): the table cache keeps a binding's tables by their words | wide tables |
| `6ed32b01a` | 2026-10-04 | superseded | perf(shader): captures share a memoized table resolution instead of copying it | wide memo (resolveTableImage) |
| `0771973a6` | 2026-10-04 | ported | fix(agc): agc_frame_replay collects its restore's writes before the loop | frame capture and replay are in (0f259798b at 90%) |
| `34f37825d` | 2026-10-04 | superseded | perf(agc): table object builds pin without sorting their padding and keep no records | wide tables |
| `639c50f40` | 2026-10-04 | not needed | perf(agc): DMA_DATA from the GDS into memory is a GPU copy | GDS DMA_DATA is no hotspot on the #639 driver (DMA_DATA under 60 ms per 10 s at the menu) |
| `5a97a31f6` | 2026-10-04 | ported | perf(agc): agc_frame_replay writes large memory deltas from several threads | frame capture and replay are in (0f259798b at 90%) |
| `ef46e7eab` | 2026-10-04 | superseded | perf(agc): table proofs and texture cache entries keep their mapping generation current | wide tables |
| `19da49664` | 2026-10-04 | superseded | perf(agc): table object builds pin without sorting their padding and keep no records | wide tables |
| `8d1012f32` | 2026-10-04 | not needed | perf(agc): DMA_DATA from the GDS into memory is a GPU copy | duplicate of 639c50f40 |
| `ccd6c26b2` | 2026-10-04 | superseded | perf(agc): table proofs and texture cache entries keep their mapping generation current | wide tables |
| `538a36d29` | 2026-10-04 | superseded | perf(agc): a table walk finds its elements' cache entries without a key lookup | wide tables |
| `1a48ebde2` | 2026-10-04 | superseded | perf(agc): a table built over the same words as a cached one takes its duplicate map | wide tables |
| `2bacb1a45` | 2026-10-04 | superseded | perf(agc): a table walk finds its elements' cache entries without a key lookup | wide tables |
| `52ba8a519` | 2026-10-04 | superseded | perf(agc): a table built over the same words as a cached one takes its duplicate map | wide tables |
| `fcbd4ee86` | 2026-10-04 | superseded | perf(agc): a batch notes a cached address space's reads once | #639 address spaces |
| `432af9c6c` | 2026-10-04 | superseded | perf(agc): a cached address space lists its mirrored regions | #639 address spaces |
| `664c39406` | 2026-10-04 | superseded | perf(agc): a batch notes a cached address space's reads once | #639 address spaces |
| `3db9aa6f8` | 2026-10-04 | superseded | perf(agc): a cached address space lists its mirrored regions | #639 address spaces |
| `0c20c3b30` | 2026-10-06 | ported | fix(ampr): report failed and empty /app0 opens and name the failed-request fence in null waits | 68% of its lines are in the branch |
| `3a9b46b97` | 2026-10-07 | ported | test(agc): thread-dimension indirect dispatches on the GPU and the CPU path |  |
| `060a736a1` | 2026-10-07 | superseded | feat(ajm): resample the decode output by the instance's ratio | upstream AJM resampler |
| `ebfdf3b01` | 2026-10-07 | superseded | feat(kernel): the AMM's donated direct memory as a backing pool | upstream AMM |
| `9f1d1b302` | 2026-10-07 | ported | fix(shader): the disk cache's Linux layout checks match the current structs | too small to measure; merge fix-up or one-liner |
| `52d400755` | 2026-10-07 | ported | test(agc): the recorder tests skip what lavapipe cannot show and run as a ctest |  |
| `5e1100517` | 2026-10-07 | not needed | feat(shader): the SRT walk skips flat slots behind branches it can rule out | no SRT walk failure or null-page read in the menu runs (upstream branch-selected descriptors cover them) |
| `7f29dcab5` | 2026-10-07 | diagnostic | feat(shader): APS5_SRT_DEBUG names what the walk evaluated at a null-page read | port on demand |
| `f2309135f` | 2026-10-07 | not needed | feat(shader): the SRT guards also cover descriptor sources | with 5e1100517 |
| `8a7633bbc` | 2026-10-07 | ported | test: the AJM resampler and thread-dimension dispatch tests build on Windows |  |
| `50b364a41` | 2026-10-07 | diagnostic | wip(wolverine): call tracer and probes for the post-intro stall | port on demand |
| `14f0eced7` | 2026-10-07 | diagnostic | wip(wolverine): tracer filters (type byte, zero base, rax/rsi dumps, image peeks) | port on demand |
| `82aa9070e` | 2026-10-07 | ported | wip(wolverine): AMM range size and hinted reservations as switches; watchpoints in the call tracer | switches ported as 558489870 (APS5_AMM_RANGE_GIB, APS5_RESERVE_HONOR_HINT); tracer watchpoints are diagnostics |
| `e4cabef82` | 2026-10-07 | superseded | wip(wolverine): APS5_IMPORT_WINDOW_SPLIT_REMAINDER splits a range's remainder into its own import window | #639 import windows |
| `d32ef2e60` | 2026-10-07 | ported | AgcDriver: import a whole windowed range for a descriptor that binds it | ported 10-10 (whole-range imports) |
| `9a5734df5` | 2026-10-07 | superseded | AgcDriver, recompiler: compute shaders with scratch, dispatch skip only for compile errors | upstream compute scratch (request compute.scratchDwords) |
| `85981ad34` | 2026-10-07 | diagnostic | AgcDriver: [texmem] sampled-cache memory report under APS5_PROFILE_DRAW | port on demand |
| `1be5b791e` | 2026-10-08 | diagnostic | fix(kernel): the reserve-caller trace builds on Linux | port on demand |
| `6671f435c` | 2026-10-08 | re-implement | feat(shader): subroutine calls (s_swappc_b64) inlined per call site | computed s_swappc / function-table subroutine calls (4 shaders left unprepared, 0x11d049aa00 and others): the old pass inlines callees from the dispatch user data, but #639 prepares shaders at registration without it |
| `ed5151a47` | 2026-10-08 | ported | feat(shader): SRC_SHARED_BASE / SRC_PRIVATE_BASE and FLAT loads through the apertures | ported 10-10 (aperture registers, FLAT through the apertures) |
| `13123c510` | 2026-10-08 | superseded | feat(shader): APS5_SRT_PHI_SELECT, descriptors chosen by a branch the SRT walk evaluates | upstream branch-selected descriptors (kept in the merge) |
| `381025e72` | 2026-10-08 | superseded | feat(agc): APS5_IGNORE_LINEAR_ATTR_CLIP accepts PA_CL_CLIP_CNTL.DX_LINEAR_ATTR_CLIP_ENA | upstream accepts DX_LINEAR_ATTR_CLIP_ENA (ClipControlMask) |
| `f494ea6ad` | 2026-10-08 | re-implement | feat(recompiler): APS5_BINDLESS_STORAGE, storage images through T# tables | bindless storage image tables (1 shader left unprepared): on the wide tables (a storage variant of WideImages) |
| `b8000908c` | 2026-10-08 | diagnostic | diag(agc): [texmem] storage images split into cached and evicted-but-held | port on demand |
| `2ed391541` | 2026-10-08 | ported | feat(agc): APS5_IGNORE_CONSERVATIVE_Z / APS5_IGNORE_POPS (DB_SHADER_CONTROL) | ported as 083a575e6 (APS5_IGNORE_CONSERVATIVE_Z / APS5_IGNORE_POPS) |
| `0694decaf` | 2026-10-08 | ported | feat(agc): APS5_DEPTH_CLEAR_DRAWS, DB_RENDER_CONTROL depth/stencil clear draws | ported as bef75f0c6 (APS5_DEPTH_CLEAR_DRAWS) |
| `29c7eeecd` | 2026-10-08 | not needed | feat(agc): APS5_SNAPSHOT_DIGEST, sampled snapshots kept as chunk hashes | RAM: flat with the run settings (APS5_DRAW_SNAPSHOT_MAX_MIB=32 and the cache sizes) |
| `3111a0e6f` | 2026-10-08 | tool/test | test(agc): bindless storage test's image_load is 2D, as its comment says |  |
| `cd49c8911` | 2026-10-08 | diagnostic | fix(agc): INDEX_BUFFER_SIZE message says whether a DISPATCH_RESET cleared it | port on demand |
| `43313d83d` | 2026-10-08 | diagnostic | diag(agc): VGT_SHADER_STAGES_EN rejections name the fields | port on demand |
| `26a362bb0` | 2026-10-08 | not needed | feat(agc): APS5_SAMPLED_FROM_CACHED_STORAGE views a cached storage image | VRAM: as a49f67a44 |
| `d2d2dc75b` | 2026-10-08 | ported | feat(agc): APS5_PS_STATE_FROM_SHADER | ported as 4476f858b (APS5_PS_STATE_FROM_SHADER) |
| `b4d5decef` | 2026-10-08 | ported | test(agc): agc_driver_flip waits 30 s before calling the driver hung |  |
| `40f257336` | 2026-10-08 | ported | fix(agc): a dispatch whose poisoned source is rebuilt names the shader when it fails | 50% of its lines are in the branch |
| `afc820716` | 2026-10-08 | re-implement | feat(shader): nested subroutine calls, null-swap returns and s_code_end | with 6671f435c (nested calls, null-swap returns) |
| `058e78be5` | 2026-10-08 | re-implement | feat(shader): trace call targets to user data across forward control flow | with 6671f435c (call targets traced through forward flow) |
| `aed011ded` | 2026-10-08 | diagnostic | chore(agc): out-of-memory line names every heap; APS5_TRACE_STORAGE_DUPLICATES | port on demand |
| `02bec7c36` | 2026-10-08 | superseded | fix(agc): free the replaced device's host imports | upstream refreshImports frees the imports of a replaced device |
| `3b8a110c2` | 2026-10-08 | diagnostic | chore(agc): APS5_TRACE_HEAPS and APS5_TRACE_BIG_ALLOCATIONS | port on demand |
| `98624fe8a` | 2026-10-08 | ported | fix(agc): drop the replaced device's entries from the texture caches | ported as f52cdaffb (replaced device textures cleared) |
| `fba076a1c` | 2026-10-08 | diagnostic | chore(agc): APS5_TRACE_SAMPLED_DUPLICATES | port on demand |
| `a31e425dd` | 2026-10-08 | diagnostic | chore(agc): APS5_TRACE_SAMPLED_DUPLICATES=<MiB> with both descriptors' levels and formats; notes | port on demand |
| `643b3460a` | 2026-10-09 | diagnostic | fix(agc): APS5_TRACE_BIG_ALLOCATIONS builds on Linux | port on demand |
| `a49f67a44` | 2026-10-09 | not needed | feat(agc): APS5_DROP_RECYCLED_TEXTURES, stale sampled images of a reused address go | VRAM: ~3.3 GB device memory at the menu, no failed allocation |
| `56dee73ca` | 2026-10-09 | not needed | feat(agc): APS5_STORAGE_LAYER_VIEWS, array slices bind views of the whole array's storage image | VRAM: as a49f67a44 |
| `97691f4f6` | 2026-10-09 | not needed | agc: block-compressed 3D textures and 3D mip tails | no 3D tiling refusal in any run; upstream tiles thick 3D textures |
| `45461318c` | 2026-10-02 | ported | feat(shader): store through flat and global addresses | 97% of its lines are in the branch |
| `cce706b1c` | 2026-10-09 | re-implement | wip(wolverine): table calls, call user-data keys, null unresolved images, plan builder iterative, call-table t | table calls with 6671f435c; its null-unresolved-images part is ported (9a86d8e25) |
| `fd90b7b1d` | 2026-10-09 | ported | agc(wolverine): linear storage results stored at once, draw snapshots capped, range and big-copy traces | ported 10-10 (APS5_EAGER_LINEAR_WRITEBACK, draw snapshot cap APS5_DRAW_SNAPSHOT_MAX_MIB) |
| `5b096da68` | 2026-10-09 | not needed | agc(wolverine): scissor window offsets applied, table-locating inputs out of the call key | upstream refuses a non-zero PA_SC_WINDOW_OFFSET visibly; no Wolverine draw is refused for it |
| `88e368965` | 2026-10-09 | ported | agc(wolverine): D16_UNORM_S8_UINT targets held as D32_SFLOAT_S8_UINT where unattachable, source failure dumps | taken in the merge (D16S8 as D32S8) |
| `f579d6d7a` | 2026-10-06 | ported | feat(recompiler): structurize loops whose exits reach different program ends | 100% of its lines are in the branch |
| `4e6e92734` | 2026-10-06 | ported | fix(recompiler): merge a loop selection where its other arm joins or leaves | 100% of its lines are in the branch |
| `6f285a583` | 2026-10-09 | ported | fix(recompiler): early program exits shared by nested selections | ported 10-10 (re-applied) |
| `4265a9dce` | 2026-10-09 | diagnostic | diag(agc): APS5_TRACE_BLEND prints each distinct blend setup per color target | port on demand |
| `ee6ce33c9` | 2026-10-09 | diagnostic | diag(agc): APS5_DUMP_IMAGE copies a sampled storage image to a file, APS5_DUMP_DRAW_ALL dumps draw requests; w | port on demand |
| `34f3e85c4` | 2026-10-04 | superseded | feat(recompiler): format loads through GPU-selected buffer descriptors | reverted by 27a706005 |
| `4545de2a1` | 2026-10-09 | diagnostic | diag(agc): APS5_TRACE_ATLAS prints draws into 1024x1024 R16_SFLOAT targets with blend, depth and stencil state | port on demand |
| `27a706005` | 2026-10-09 | superseded | Revert format loads through GPU-selected buffer descriptors (34f3e85c) | revert of 34f3e85c4 |
| `eb4db899f` | 2026-10-09 | ported | fix(agc): uniform fills over a whole depth or stencil plane clear that plane of the depth surface | taken in the merge (depth plane fills) |
| `646051662` | 2026-10-09 | ported | fix(agc): a depth surface made after its planes were filled starts with those fills | taken in the merge (fills before the surface exists) |
| `b8d0761c2` | 2026-10-09 | ported | diag(agc): APS5_FAST_LAUNCH_PROBE lets GS_FAST_LAUNCH=1 draws through decode and traces them | superseded: GS_FAST_LAUNCH draws are taken by default since the 10-10 port |
| `e5ac5266e` | 2026-10-09 | ported | perf(agc): bindless table cache keeps 64 tables (APS5_TABLE_CACHE_ENTRIES); fast-launch mesh draws in wave32;  | fast-launch wave32 mesh ported 10-10; its table cache is superseded by the wide tables |
| `25a773897` | 2026-10-06 | ported | agc: memoize the SRT walk in a flat per-value array | 82% of its lines are in the branch |
| `61771d279` | 2026-10-09 | superseded | perf(agc): mesh records recompile only stages whose words changed; resource cache defaults 4096 entries / 14 G | #639 mesh records |
| `9f724aa29` | 2026-10-09 | superseded | perf(agc): resource cache defaults back to 1024 entries / 6 GiB pinned | resource cache defaults (#639 caches) |
| `b6f9f526d` | 2026-10-09 | ported | feat(agc): GS_FAST_LAUNCH=1 point-per-subgroup geometry is taken by default | ported 10-10 (GS_FAST_LAUNCH point-per-subgroup) |
