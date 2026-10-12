# night/wolverine progress

## Night 2026-10-09 (queue section C)

Built in `build-wolverine` (lavapipe). The branch did not build on Linux: `3b8a110c`'s `APS5_TRACE_BIG_ALLOCATIONS`
used the Windows `MAX_PATH` (`643b3460` fixes it).

| Commit | Item | Switch | What the test proves |
|---|---|---|---|
| `643b3460` | build | none | `VulkanDevice.cpp` compiles on Linux again |
| `a49f67a4` | C1 | `APS5_DROP_RECYCLED_TEXTURES=1` | `agc_driver_recycled_textures` (+ `_drop`): three formats over one address with the contents changed between them leave 3 cached textures (768 bytes) without the switch, 1 (256 bytes) with it; two formats over unchanged memory are both kept either way |
| `56dee73c` | C2 | `APS5_STORAGE_LAYER_VIEWS=1` | `agc_driver_storage_layer_views` (+ `_on`): writes through a whole-array descriptor and two slice descriptors land in the right layers either way; 3 storage images without the switch, 1 with it |
| - | C3, C4 | - | analysis only (below) |

Tests run here: the new tests and their neighbours (`recycled_textures`, `sampled_from_cached_storage(_on)`,
`bindless_storage`, `image_atomics`), all pass; the full suite at the end of the night: 174 of 177, the three known
lavapipe failures (`gds_append`, `sdwa_compare_scan`, `vopc_compare`).

**For the day session (RTX 5080), in order:**
1. The menu load with `APS5_DROP_RECYCLED_TEXTURES=1 APS5_STORAGE_LAYER_VIEWS=1` (then each alone): the VRAM totals at
   the wall ("textures" 9846 MiB, "storage images" 2922 MiB on 10-08) should fall, and the menu should get past frame
   ~881. `APS5_TRACE_SAMPLED_DUPLICATES=16 APS5_TRACE_STORAGE_DUPLICATES=1` show what is still duplicated.
2. Check the menu's frames with both switches for wrong textures (a dropped image still in use would show as a
   missing or stale texture; a slice view would show as a wrong layer).
3. Dump the pixel and vertex shaders at the menu load (`APS5_DUMP_SHADERS=1`) for C3's count of `Private` arrays.
4. Dump a live dispatch of `11d049aa00` (C4): the table pointer and the first 64 entries of its function table.

**Left:** C3 for the graphics stages (needs the live dump), C4's implementation. Queue item C4's second half, the
`agc_driver_recorder` failure ("a draw snapshot outlived a registry mutation", an earlier night): it passes on this
branch now (in tonight's full suite and run alone), so nothing is left there.

**C1: stale texture images of recycled addresses** (`a49f67a4`, `APS5_DROP_RECYCLED_TEXTURES=1`, off by default).
When the sampled cache makes a new CPU snapshot at an address, every other snapshot entry at that address whose
contents no longer match guest memory (the write watch cannot prove it unchanged, and a byte compare with its snapshot
fails) is dropped from the cache, and the resource-cache templates that pin its image are removed too
(`ResourceCache::DropBindings`, by the sorted image lists the cache already keeps per template). The images are then
held only by recorded work and go when it completes. An alias of unchanged bytes in another format (the same surface
read as two formats) is kept. Not covered by the test: the template half (the test's dispatches do not insert into
the shared resource cache, `PinnedBytes` is 0 there); it is a filter over the same pin lists `Insert` maintains.
*Day check:* the menu load with `APS5_DROP_RECYCLED_TEXTURES=1 APS5_TRACE_SAMPLED_DUPLICATES=16`: the `[sampled-dup]`
lines for BC7-over-BC5 / SRGB-over-UNORM pairs should stop repeating, the VRAM "textures" total at the wall should
fall (9846 MiB on 10-08), and the menu should get further than frame ~881.

**C2: per-slice storage images** (`APS5_STORAGE_LAYER_VIEWS=1`, off by default). A storage descriptor of a 2D array
or cube is looked up as the whole surface: base array 0 and the largest last array of a cached image of that surface
(same address, extent, mips, tile mode, dimension and storage format), and it binds a layer-range view
(`StorageTexture::LayerView(mip, baseArray, layers, flat)`; flat is the one-layer 2D view of a 2D shader over an
array). The sampled-from-storage lookup does the same, and `Texture::CanCopyFrom` accepts a source with more layers
(the sampled view selects its own range). The guest layout is layer-major (`DescribeSurface`: `layerBytes * layers`),
so a slice descriptor's memory is a prefix of the whole array's. Test `agc_driver_storage_layer_views` (+ `_on`): a 4-layer
R32 array, a kernel that loads, adds one and stores through a 2D image; dispatched over the whole array, over slice 2
(base = last = 2), over slice 3 (base 3, last 3) and over slice 2 again: every layer holds its expected value either
way, and the dispatches made 3 storage images without the switch, 1 with it. (Both modes print lavapipe's "guest memory
changed while GPU results were pending" on a repeated dispatch over one image; it appears with the switch off and
without slices too.) Not handled: a slice image made before its whole array is cached stays as it is (the next
whole-array lookup makes the full image; the slice image is flushed first by the creation's `FlushPending`), and
render-target attachments keep their own key. The lookup scans the storage cache once per layered storage element.
*Day check:* the menu load with `APS5_STORAGE_LAYER_VIEWS=1 APS5_TRACE_STORAGE_DUPLICATES=1`: the `[storage-dup]`
lines whose words differ only in word 6 (base array) should stop, and the VRAM "storage images" total (2922 MiB on
10-08) should fall.

**C3: per-invocation arrays** (analysis). The 163 saved requests in `testdata/wolverine-menu/reqs` are all compute
shaders. Their headers (read with `agc_shader_replay` from `night/merge`, which prints them) declare **0 dwords of
scratch per thread** in every one inspected (the first 31; the run was cut by memory, see below), and LDS of 0 to
1152 dwords (117 none, 19 x 128, 10 x 256, 6 x 384, 5 x 512, 2 x 640, 1 x 896, 1 x 1024, 2 x 1152). Compute LDS is
Workgroup memory, not a per-invocation array, so these shaders reserve no NVIDIA local memory through our scratch
array. The 38 that compile offline declare no `Private` or `Function` array variables at all. The LDS emulation outside
compute (KytyPS5's case) does not occur in this set: the pixel and vertex shaders are not in the saved requests, so the
day session should list them live (`APS5_DUMP_SHADERS=1` at the menu load, then count `Private` arrays in their
SPIR-V). Found on the way: compiling `shader_11d0310700.req` took over 14 GB in SPIRV-Tools' global redundancy
elimination; `night/merge` `c1436597` replaces that pass (1.5 GB, 67 s); this branch still has the old pass.

**C4: the indirect call in `11d049aa00`** (analysis, no change). With `APS5_SUBROUTINE_CALLS=1` the request stops at
"s_swappc_b64 at program counter 0x2470: indirect call (the target in s14 is not user data, an immediate or a program
counter offset)". The code before it (0x2424-0x2470) is a waterfall loop over a per-lane function index:
- `v1 = v11 + v13` per lane; `v_readfirstlane vcc_lo, v1`; `v_cmpx_eq_u32 exec, vcc_lo, v1` keeps the lanes with that
  index; `vcc_lo <<= 4`.
- `s_load_dwordx2 s[8:9], s[12:13], 0`: the table pointer, read from memory that user data s[12:13] points at (two
  levels from user data).
- `s_load_dwordx4 s[8:11], s[8:9], vcc_lo`: a 16-byte entry, {target lo, target hi, two argument dwords}; the target
  goes to s[14:15] and the arguments to s[16:17], then `s_swappc_b64 s[14:15]`; after the call the loop restores exec
  from v143 lane 10 and repeats for the remaining indices.

The request has 2 memory regions (the code and the user-data block), not the table: its entries cannot be listed
offline, and their count depends on v11 + v13 (per lane, data dependent). Two ways to support it, neither started:
1. *Bind-time switch.* The driver reads the table pointer and the table at dispatch, collects the distinct targets
   of the entries up to a bound (stop at the first entry whose target is outside guest code), and the shader is
   specialized with an `OpSwitch` over those targets (the recompile key holds them). Needs the callee code in the
   request (the driver already adds callee code for user-data targets) and a fallback for an index past the bound.
2. *Run-time dispatch.* All candidate callees inlined as one SPIR-V function with a selector; same table read, but
   one compile per table content instead of per index set.
Option 1 fits the existing callee-capture path; the day session should first dump a live dispatch of `11d049aa00`
(table pointer and the first 64 entries) to see how many distinct targets the menu uses.

## Day 2026-10-08 (RTX 5080, live runs): subroutine calls finished, menu still on the memory wall

Run switches as on 10-07 plus the night's: `APS5_SUBROUTINE_CALLS=1 APS5_SRT_PHI_SELECT=1 APS5_BINDLESS_STORAGE=1
APS5_IGNORE_LINEAR_ATTR_CLIP=1 APS5_IGNORE_CONSERVATIVE_Z=1 APS5_IGNORE_POPS=1 APS5_DEPTH_CLEAR_DRAWS=1
APS5_SNAPSHOT_DIGEST=1 APS5_SAMPLED_FROM_CACHED_STORAGE=1 APS5_PS_STATE_FROM_SHADER=1`. With all of them the
intros run and the menu load reaches frame ~852-881, then stops on GPU memory as before.

Fixed (commits on `wolverine-next`):
- `40f25733`: a dispatch whose poisoned source is rebuilt names the shader when the rebuild fails. Before, the
  unprefixed recompile error reached presentation and crashed the run.
- `afc82071`, nested subroutine calls:
  - A callee's calls are inlined depth first right after its copy, up to 8 deep. Recursion is refused.
  - Each call records every register it and its nested calls write, so a later call's target can still be
    traced back past it.
  - `s_swappc_b64` with a null destination through the return pair is a return (the compiler emits it in place
    of `s_setpc_b64`).
  - A callee that calls further may save and restore its return pair.
  - SOPP 31 `s_code_end` decodes (padding after the code).
  - `7ff7e12c6c00` now compiles live.
- `058e78be`: a call target that hits a branch target while traced back may still be user data. This holds in
  the main program when nothing before that point writes the register (earlier calls count by what their copies
  write) and no branch from there on jumps back before it. `7ff7e1307100` (calls through s[8:9] after a few
  `s_cbranch_execz`) now inlines. In replay it stops at the SRT walk, where the dump lacks the slot memory a live
  dispatch captures.

Still open:
- **Real indirect call:** `11d049aa00` at 0x2470. `s_load_dwordx2 s[8:9]` from s[12:13], then `s_load_dwordx4
  s[8:11]` from s[8:9] + `vcc_lo`, where `vcc_lo` = `v_readfirstlane` of v1 << 4. That is a function table indexed
  per wave. Inlining it needs every table entry (a switch over the loaded target) or a run-time dispatch.
- **Memory wall** (same numbers as 10-07). Heap 1: budget 15418 MiB, usage 15673 MiB. Of that:
  - host imports 10840 MiB (1755 imports), host buffers 1435 MiB, shadow slabs 296 MiB;
  - the one whole-range import (the streaming heap `0x1210000000+0xab000000`, 2736 MiB) leaves 8.1 GB imported
    by windows before it.

  In VRAM: textures 9846 MiB, storage images 2922 MiB.

  Heap 1 is shared by three budgets, each a share of the 15.4 GB heap:
  - imports: 3/5 (9250 MiB), which whole-range imports go past;
  - heap mirrors: 1/5 (3083 MiB, `APS5_HEAP_MIRROR_MIB`);
  - host buffers and shadow slabs: whatever is left.

  Mirrors held only 537 MiB at the wall. Even so, ~2.5 GB of heap-1 usage is in none of the tracked categories
  (driver-internal memory, or device memory placed in system memory?).

  Tried today, both worse:
  - `APS5_HOST_IMPORT_WINDOW_MIB=256` (1024 is the default). The AMM range still took 29 windows (7.3 GB). Imports
    hit their budget during the intros, host-buffer copies grew to 2.9 GB, intros ran at 4 fps (20 before), and
    allocations failed from frame ~250.
  - `APS5_HOST_IMPORT_MIB=6144`. Refused imports fall to heap mirrors, and the streaming heap's mirror chunk went
    past the mirror budget: FATAL abort during the intros.

  The out-of-memory line now also prints the other heaps and how much went to another heap
  (`[gpumem] ... heap 0 budget ..., usage ...; N MiB placed in another heap so far`). At the wall:
  - heap 0 is at 14561 of 14821 MiB, so VRAM is full too;
  - 0 MiB of device allocations were placed in heap 1, so the ~3 GB untracked in heap 1 is not spilled textures.

  **Duplicate storage images are per-slice bindings** (`APS5_TRACE_STORAGE_DUPLICATES=1`, the first 64 cases,
  `[storage-dup]` lines):
  - 59 of 64 bind one slice of an array that is already cached whole: base array = last array = k (k = 1..7).
    Examples: two 96x96 format 97 arrays of 8 slices at `0x1184790000` / `0x11f0e5c300`, 28 cases each.
  - The 256x256 format 122 cube at `0x11887b0000` (6 faces, 6 mips) is bound face by face as 2D (dimension
    word 0x207000 against 0x307000), plus once as a plain 2D image of face 0.
  - `GetGeometry` gives such an image `lastArray + 1` layers from layer 0 and views layer `baseArray`. Binding
    slices 1..7 one by one therefore allocates 2+3+...+8 layers next to the 8-layer original. The copies are
    separate images over the same memory, kept in step only through guest memory.

  The fix: `StorageKey` without base array and with the largest last array. `lookupStorageTexture` returns the
  containing image whenever its key matches apart from those fields, and the binding gets a view of
  `[baseArray, lastArray]`. That needs layer-range views next to `View(mip)` / `FirstLayerView(mip)` (`Texture.hpp`)
  and in every caller. The cube-as-2D case needs a 2D-array view of the cube image.

  **Found and fixed (`02bec7c3`): the "untracked" 3 GB in heap 1 was a leak.** `APS5_TRACE_HEAPS=1` (heap usage
  against the tracked categories every 10 s) showed the gap appearing at once, ~3.15 GB in one 10 s window, early in
  the intros, and staying constant. `APS5_TRACE_BIG_ALLOCATIONS=64` showed the cause:
  - the first dispatches run before the first flip, so `Dispatch.cpp` makes a headless `VulkanDevice`;
  - `Presentation.cpp` replaces it with the windowed one and keeps the old device in `replacedDevices` for the
    whole run;
  - on the device change, `refreshImports` dropped the old device's imports (110 + 1734 + 1024 + 395.5 MiB) from the
    count, but never destroyed their buffers or memory, then imported the same ranges again on the new device.

  They are now freed through the old device (idle after `PrepareForReplacement`). `APS5_KEEP_REPLACED_IMPORTS=1`
  restores the old behaviour. `gta-v/fidelity-main` already had the same fix (`state.destroyBuffer` /
  `state.freeMemory` in `refreshImports`); wolverine-next branched off before it. Take fidelity-main's form when the
  branches meet.

  Run with the fix:
  - heap 1 matches the tracked total and peaks at ~12.5 GB, where it used to sit at 15.6 GB;
  - the menu load no longer fails in heap 1. It fails in VRAM instead: textures grow to 13.5 GB (9.3 before),
    heap 0 usage reaches 18.8 GB against a 15 GB budget, device allocations go to heap 1 ("placed in memory
    type 0"), and texture allocations fail again at frame ~838.

  The old device also keeps ~0.9 GB of heap 0 (the heap-0 gap: device usage minus tracked). Its textures, buffers
  and pools are still referenced by it. Freeing the whole replaced device (or never making the headless one: open
  the windowed device at the first dispatch) would return that too.

  `98624fe8`: the replaced device's texture-cache entries (296 storage images, 843 sampled textures) are now
  also dropped at the replacement (`ForgetDeviceTextures`, `APS5_KEEP_REPLACED_TEXTURES=1` keeps them). With that,
  storage images at the wall fell from 2.9 to 1.5 GB, but textures rose from 13.5 to 14.6 GB: the menu's one build
  takes whatever room there is. `APS5_TEXTURE_CACHE_MIB=6144` changes nothing (13.46 GB textures; the build pins
  them).

  **The real wall is now the system commit limit, not a Vulkan heap.**
  - Texture allocations go to heap 1 when VRAM is out (2000+ "placed in memory type 0"). Heap 1 still had ~3 GB
    free when a 2.7 MiB device buffer failed in every memory type.
  - At that moment Windows had 0.8 GB of commit free out of 89.8 GB (32 GB RAM plus a grown pagefile), and eboot
    had 27.3 GB private.
  - WDDM backs video memory with commit, so ~16 GB of textures and storage images count on top of the guest memory
    and the CPU snapshots.
  - Two minutes later presentation failed with `vkQueueSubmit recorder: Vulkan result -4` (device lost), then
    abort.

  **Sampled textures are doubled too** (`APS5_TRACE_TEXTURE_MEMORY=1` at the wall: 6425 host images, 16.1 GB over
  9.8 GB of guest addresses, host/guest 1.65, against 1.21 on 10-07). Many of the largest surfaces hold two snapshot
  images of one shape, e.g. `0x299a1c000` 4096x4096 f177, 128 MiB for 64 MiB of guest memory.
  `APS5_TRACE_SAMPLED_DUPLICATES=1` logs, 48 times, a new snapshot texture at the address of a cached one: the T#
  word XORs, whether the swizzles differ, and why the shared-snapshot lookup (`cache.surfaces`, which views one
  image under another descriptor) missed. In 43 of 48 cases there was no surface entry, because
  `SnapshotSurfaceKey` differs:
  - **format** (word 1 bits 20-28): the same texel layout read in another number format (e.g. 36 against 56,
    UNORM/SRGB style), often with another swizzle (word 3 `0xc00`/`0xc88`/`0xd80`);
  - **image type** (word 3 bit 30: 2D against 2D array) at the same extent and format;
  - **extent** (word 2) at the same address: different surfaces, not duplicates.

  Tried and reverted, both without effect on the wall (textures still ~13 GB, host/guest ~1.64):
  - sharing only the BC pairs (169/170 ... 181/182) through a MUTABLE_FORMAT image;
  - sharing by texel layout (BC pair, or element size for uncompressed color) with 2D = one-layer 2D array.

  The largest menu duplicates have the **same** format and shape (`0x29af7c000` f177 4096x4096 twice,
  `0x2703a4000` f182 8192x4096 twice). Their keys must differ in what the texmem shape leaves out: mip count, base
  level or array range. Or one is a fresh snapshot after a write, made while the build still holds the old one.
  The 48 traced cases came from the intros, before these. Next: limit `APS5_TRACE_SAMPLED_DUPLICATES` to textures
  of at least 16 MiB, and print the surface key fields (mips, base level, layers) of both. If it is the mip count,
  view a shorter chain from the full image (the key without `mipCount`, the view's `levelCount` from the
  descriptor). If it is a fresh snapshot, let the old copy go once the build that holds it completes.

  **Answer (`APS5_TRACE_SAMPLED_DUPLICATES=16`, the value is the smallest snapshot in MiB; it prints both
  descriptors' mips/levels/arrays/format):** the large "duplicates" are mostly **stale images of recycled
  addresses**, not two views of one texture.
  - 4096x4096 at `0x3a5d18000`: new BC7_UNORM (181), cached BC7_SRGB (182).
  - At `0x3a7278000`: new 181, cached BC5 (177), with another swizzle. The game's streaming pool reuses the memory
    for a different texture.
  - At `0x3c8378000`: same format, "contents or mappings changed", so a new snapshot after the texture was
    rewritten.
  - The 160 MiB 8192x10240 format 7 shadow map at `0x112f900000` is held twice, once sampled with depth compare.
    Depth-compare textures never take the shared-snapshot path (`!depthCompare` guards it).

  Textures reach ~13 GB against an ~8 GB cache budget, so the old images are held outside the texture cache. The
  likely holder is the resource-cache entries (descriptor sets and bindless table bindings pin their textures past
  the budgets). Next:
  - when a snapshot is made at an address whose cached entries no longer match (other format, or contents
    changed), drop those entries and the resource-cache entries that bind them, so old images go once their last
    recorded use completes;
  - the depth-compare copy cannot just view the color snapshot. Compare sampling needs a depth-format view, and
    Vulkan does not view one image as both color and depth. Either keep only the depth image when both are used
    (sample it without compare for the plain reads), or emulate the compare in the shader for one of the two.

  The general design, if format variants do turn out to matter at the menu:
  - key the shared snapshot by texel layout (bytes per element and block size, Vulkan compatibility class) instead
    of the guest format;
  - treat 2D and single-layer 2D array as one;
  - create snapshot images with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (with a format list where the driver
    benefits);
  - view them in each descriptor's format and swizzle (the `Texture(context, base, resource, components)`
    constructor already does the view).

  So the menu load needs less memory overall:
  - fewer host textures in the menu's build (lazy bindless elements: only the elements a dispatch reads);
  - no CPU snapshots of sampled textures (5-7 GB on 10-07);
  - storage slices as views (above).

  The levers left:
  - fewer guest bytes that must be GPU-visible: the 2.7 GB streaming-heap descriptor, and storage images duplicated
    per dynamic-resolution extent (the resource cache's entries pin them past alias eviction);
  - copies in device memory instead of heap 1.
- `agc_driver_recorder` fails on this branch with "a draw snapshot outlived a registry mutation"
  (`tests/Recorder.cpp:1098`). It is unrelated to the shader commits (draw snapshot reuse); it was not checked on
  the base.
- `night/perf` (GTA) still carries the `SubmitThread` exit hang that fidelity-main fixed: the static thread was
  joined during DLL detach. fidelity-main's `e9aeb10f` never destroys it.

## Night 2026-10-08 (queue section C)

Base `c9571635` (the local worktree was fast-forwarded to it first). Built in `build-wolverine` on lavapipe. The test
data is `testdata/wolverine-menu/`. Every `agc_*`/`shader` test passes except `agc_driver_gds_append`,
`agc_driver_sdwa_compare_scan` and `agc_driver_vopc_compare`, which fail at the base commit too (lavapipe's 8-lane
subgroups).

### Commits

| Commit | Item | Switch | What the test proves |
|---|---|---|---|
| `1be5b791` fix(kernel): reserve-caller trace builds on Linux | - | none (build fix) | `libkernel` compiles on Linux again (`GetModuleHandleA` was unguarded) |
| `6671f435` feat(shader): subroutine calls | C1 | `APS5_SUBROUTINE_CALLS=1` | `agc_driver_subroutine_calls` (new; lavapipe): (1) a user-data callee in its own code region, called twice, so two copies and two returns; (2) a callee after `s_endpgm` reached by `s_getpc_b64` + `s_add_u32`/`s_addc_u32`; (3) a callee outside the request is a clear error |
| `ed5151a4` feat(shader): SRC_SHARED_BASE / SRC_PRIVATE_BASE | C3 | none (was a hard error) | `agc_driver_flat_apertures` (ported from gta-v `0e45466f`, adapted): addresses built from `src_shared_base` / `src_private_base` read LDS (filled by `ds_write_b32`) and scratch (filled by `scratch_store_dword`) back through `flat_load_dword` |
| `13123c51` feat(shader): APS5_SRT_PHI_SELECT | C2 | `APS5_SRT_PHI_SELECT=1` | `agc_srt_phi_select` (new; ctest runs it with and without the switch): a formatted store through a V# whose dwords 0-1 come from user data 0-1 or 2-3 by a flag in user data 4. Without the switch the build refuses it as before; with it the bound V# follows the flag both ways |
| `381025e7` feat(agc): APS5_IGNORE_LINEAR_ATTR_CLIP | C5 | `APS5_IGNORE_LINEAR_ATTR_CLIP=1` | `agc_driver_graphics` (bit 24 of PA_CL_CLIP_CNTL still refused) and `agc_driver_graphics_linear_attr_clip` (accepted with the switch) |
| `f494ea6a` feat(recompiler): APS5_BINDLESS_STORAGE | C4 | `APS5_BINDLESS_STORAGE=1` | `agc_driver_bindless_storage` (new; lavapipe): four 32x1 R32_UINT images in a T# table, a key read from a buffer, then `image_load`, +1, `image_store` through the T# at that key (Wolverine's encodings). Only the keyed image changes, each texel by one, after the write-back to guest memory |
| `2ed39154` feat(agc): APS5_IGNORE_CONSERVATIVE_Z / APS5_IGNORE_POPS | C5 | `APS5_IGNORE_CONSERVATIVE_Z=1` (exact), `APS5_IGNORE_POPS=1` (approximation) | `agc_driver_graphics`: DB_SHADER_CONTROL bits 13, 14, 16 refused without their switches, bit 1 (stencil export) always; `agc_driver_graphics_db_shader_hints` (new ctest, both switches): accepted |
| `0694deca` feat(agc): APS5_DEPTH_CLEAR_DRAWS | C5 | `APS5_DEPTH_CLEAR_DRAWS=1` | `agc_driver_depth_clear_draw` (new, lavapipe): a clear draw to 1.0 over the left half of a D32 surface at 0.0, then a z = 0.5 triangle with LESS: exactly the left half is drawn. `agc_driver_graphics_depth_clear_draws` (new ctest): DB_RENDER_CONTROL 1 / 2 / 3 decode into the clear flags and values, 4 (copy) is still refused |
| `29c7eeec` feat(agc): APS5_SNAPSHOT_DIGEST | C6 (d) | `APS5_SNAPSHOT_DIGEST=1` | `agc_driver_sampled_snapshot` (new, lavapipe): sampled reads see a CPU write (new snapshot) and an unchanged texture is reused (no snapshot); `agc_driver_sampled_snapshot_digest` (new ctest): the same with chunk hashes; it fails if the digest always matches |
| `b8000908` diag(agc): `[texmem]` storage images cached / evicted-but-held | C6 | `APS5_TRACE_TEXTURE_MEMORY=1` (existing) | the new line prints (bindless storage test with the trace switch) |
| `cd49c891` fix(agc): the INDEX_BUFFER_SIZE message names a DISPATCH_RESET | C5 | none (an error message) | `agc_driver_pm4`: "never set on this queue", "the last DISPATCH_RESET cleared size 8" after an `INDEX_BUFFER_SIZE` then a reset, and the draw resolves once the size is set again |
| `43313d83` diag(agc): VGT_SHADER_STAGES_EN rejections name the fields | C5 | none (an error message) | `agc_driver_graphics`: 0x482030 is refused with "(GS_FAST_LAUNCH=1)", 0x3000 with "(VS_WAVE_ID_EN=1)" |
| `26a362bb` feat(agc): APS5_SAMPLED_FROM_CACHED_STORAGE | C6 (b) | `APS5_SAMPLED_FROM_CACHED_STORAGE=1` | `agc_driver_sampled_from_cached_storage` (new, lavapipe; host imports and the dispatch-level caches off): a stored surface, written back, then sampled by another kernel reads the stored values. Without the switch that makes one snapshot, with it none (`_on`). A CPU write to one texel reaches the next sample either way. With the lookup forced to "not cached", `_on` fails |
| `d2d2dc75` feat(agc): APS5_PS_STATE_FROM_SHADER | C5 | `APS5_PS_STATE_FROM_SHADER=1` | `agc_driver_pixel_header_state` (new, no GPU): `draw_failure_1.b64` still fails as dumped; the pixel shader header's context registers applied to a cleared context give NUM_INTERP 1, COL_FORMAT 0x44, DB_SHADER_CONTROL 1, and the request recompiles with the pixel stage decoded from it; a context that already has the interpolant keeps its own values. Fails with the apply step disabled |

### Per item

- **C1, subroutine calls.**
  - *How.* `InlineSubroutineCalls` (`ControlFlow/src/SubroutineCalls.cpp`) runs right after the decode.
    - It follows each `s_swappc_b64`'s target back through scalar moves to user data, an immediate, or `s_getpc_b64`
      + add.
    - It decodes the callee's reachable code from the request's memory regions and appends one copy per call site
      after the caller's code.
    - The graph sees the call and the copy's `s_setpc_b64` return as plain branches. `s_getpc_b64`/`s_swappc_b64` in
      a copy add the copy's guest offset.
    - Unsupported, each with a clear error: nested calls, recursion, dynamic jumps in a callee, a callee writing its
      return registers, and indirect targets.
  - *Driver side.* With the switch, a compute dispatch whose code has the call opcode gets the registered shader code
    that a user-data pointer points at added to its request. The recompile cache key of such programs holds the user
    data.
  - *The two requests.* `7ff6bd996c00` and `7ff6bd9d7100` now resolve their targets (user data s[0:1] and s[8:9]) and
    stop at "the code of callee 0x11d039d800 is not in the request": they were dumped before the driver added callee
    code.
  - *Another request.* `11d049aa00` (from C3's list) makes a real indirect call through a function table indexed at
    run time; it stays unsupported, as the queue allows.
- **C2, phi-chosen descriptors.**
  - *Cause.* `7ff6bd9d4400` / `7ff6bd9da500` choose their V# base by `s_cmp_lg_u32 0, s2`, a branch on user data.
  - *Fix.* With the switch, a two-way phi in a failing descriptor's dwords whose edges trace back to one branch's two
    sides becomes a select on its condition, evaluated at bind time.
  - *Result.* Both requests pass resource analysis. They then stop where the dump lacks the SRT memory their slots
    read; a live dispatch captures it.
- **C3, aperture source operands.**
  - *What they are.* 235-238 decode as the apertures' high dwords: 0x80000000 / 0x8000ffff shared and
    0x70000000 / 0x7000ffff private. These are the values the FLAT routing (ported from gta-v) reads.
  - *Results.* `11d040e200` gets past the decode and stops where the dump lacks descriptor memory. `11d049aa00`
    reaches the indirect call above.
  - *Gap.* This branch still has no FLAT store emitter, so stores through the apertures stay a compile error.
- **C4, bindless storage image tables** (`11d0395500`).
  - *What the kernel does.* Its stored image is not a per-lane table entry. The T# is `s_buffer_load_dwordx8` from a
    table V# (user data 0-3) at (a word of SRT memory) * 32: one key per dispatch. It reads the image with
    `image_load` and writes it with `image_store`. Its other table T#s are sampled, and three are also read with
    `image_load`.
  - *Fix (switch on).*
    - A storage-class access whose T# the SRT walk can evaluate (`DirectlyBindable`) is no longer planned as a
      table. `GetHandle` binds the one image the key names, like a directly loaded T#, with the usual storage
      write-back. Its refusal of image dwords read from a constant buffer is skipped for that case only.
    - `image_load` through a real (per-lane) table is planned like a sampled access (`EmitReadOp` fetches from the
      slot's sampled descriptor).
    - Stores and atomics through a per-lane table are still refused; the message now names the op.
  - *The request.* `11d0395500` now passes resource analysis. Like 119 of the 163 menu requests, it then stops where
    the dump lacks the SRT memory that bind-time evaluation reads. No other request changes with the switch.
  - *Seen in the test.* All four table entries got sampled snapshot textures although only the keyed one is bound.
    Something scans the T#s in the table buffer: with ~3250 table entries at the menu, that may be part of C6's
    memory. Not looked into.
- **C5, the failing draws** (the log: 7 PA_CL_CLIP_CNTL, 6 interpolation, 5 depth export / coverage, 4 DB_RENDER_CONTROL,
  4 VGT_SHADER_STAGES_EN, 4 sampler from a constant buffer, 1 each structurizer merge, index size, OOM, depth-only,
  array-slice views).
  - *Interpolation (6).* All six dumps are the same draw: a 3-vertex DRAW_INDEX_AUTO to color target 0x114c6c0000,
    vertex program 11d00c3300, pixel program 11d015f100. Decode them by cutting the text after the base64 (the dump
    appends ` [DRAW_INDEX_AUTO, ...]`); `agc_shader_replay --stage <file>` then prints the decoded pixel stage, the
    linked programs and the shader header.
    - The pixel shader's header lists nine context registers: SPI_PS_IN_CONTROL (0x1b6) = 1, SPI_PS_INPUT_ENA/ADDR
      = 2, SPI_SHADER_Z_FORMAT = 1, SPI_SHADER_COL_FORMAT = 0x44, SPI_BARYC_CNTL = 0x01000000, DB_SHADER_CONTROL = 1
      (Z export), CB_SHADER_MASK = 0x11, PA_SC_SHADER_CONTROL = 0. Its code does `v_interp_p1/p2` on attribute 0
      (x, y) and exports depth, MRT0 and MRT1, which matches the header.
    - The draw's context holds none of them: NUM_INTERP 0, no depth export, COL_FORMAT 0, inputs at the default.
      So it is not only 0x1b6 that is missing. The whole set of the pixel shader's context registers never reached
      the context, or something cleared the context after it was written (a CLEAR_STATE, or a context-state op
      0/3, which resets to `InitialContextRegisters`). The AGC HLE never writes 0x1b6 itself;
      `sceAgcLinkShaders` / `sceAgcCreateInterpolantMapping` only produce SPI_PS_INPUT_CNTL_n.
    - `APS5_PS_STATE_FROM_SHADER=1` (`d2d2dc75`): when the header's NUM_INTERP is above the context's, the draw
      decode writes the header's context registers into a copy of the queue state and prints
      `[gpu] pixel shader 0x... : the context lacks its header's registers` (first 20). This is a workaround. If
      the game sets SPI_PS_INPUT_CNTL_0 after the shader, the link still comes from the context.
    - For the day session: run the menu with `APS5_TRACE_SHADER_REGS=11d015f100 APS5_TRACE_CONTEXT_STATE=1`. The
      first prints the header's registers when the shader is registered. The second prints each context-state op,
      but it lists indirect register writes (`0x9f`) only while a context is pushed. To find the real cause, also log
      every write to 0x1b6 in `Pm4.cpp` `writeRegister`, with its opcode. Then check whether a `0x9f` packet with the
      header's `cx_registers` address (0x1200036648) runs before the draw, and whether a context-state op or a
      CLEAR_STATE comes between it and the draw.
  - *PA_CL_CLIP_CNTL=0x01000000 (7).* That is `DX_LINEAR_ATTR_CLIP_ENA` alone, accepted with the switch.
  - *DB_SHADER_CONTROL (5).* 0x2801 / 0x2841 / 0x32641 set CONSERVATIVE_Z_EXPORT (a depth hint), accepted with
    `APS5_IGNORE_CONSERVATIVE_Z=1` (exact). 0x11450 / 0x32641 set PRIMITIVE_ORDERED_PIXEL_SHADER (raster order),
    accepted with `APS5_IGNORE_POPS=1`. That is an approximation: the exact form is fragment shader interlock
    around the pixel shader. 0x43 exports stencil, which the backend cannot (stays refused).
  - *Sampler from a constant buffer (3-4).* The sampler index is per pixel (from an interpolant, scalarized with
    `ReadFirstLane`): a real per-lane sampler table. No request of these draws was dumped; not attempted.
  - *DB_RENDER_CONTROL (4: values 1, 2, 3).* Depth / stencil clear draws. With `APS5_DEPTH_CLEAR_DRAWS=1` the draw is
    recorded as `vkCmdClearAttachments` with DB_DEPTH_CLEAR / DB_STENCIL_CLEAR over its scissor within its viewport,
    in its render pass, and counts as a depth (stencil) writer. It assumes the draw covers that rectangle, as clear
    draws do (a full-viewport rectangle); a clear draw of a smaller primitive would clear too much.
  - *INDEX_BUFFER_SIZE unset (1).* The draw is a `DRAW_INDEX_INDIRECT_MULTI` to the same color target as the
    VGT_SHADER_STAGES_EN draws. The CP clamps indexed indirect draws to INDEX_BUFFER_SIZE, so the size must come from
    an earlier `INDEX_BUFFER_SIZE` packet (`sceAgcDcbSetIndexCount`). One suspect is in the driver: the custom
    DISPATCH_RESET NOP (`sceAgcAcbResetQueue`) resets the whole `QueueState`, the index size and base included. What
    the real reset clears is not known here, so nothing is changed. The message now says which case it is (`cd49c891`):
    "never set on this queue", "not set since the last DISPATCH_RESET" or "the last DISPATCH_RESET cleared size N",
    with the queue's reset count and whether INDEX_BASE is set. If it names the reset, keep the index state across
    DISPATCH_RESET (it is emitted by the compute command buffer API).
  - *VGT_SHADER_STAGES_EN=0x482030 (4).* Decoded with the GFX10 field layout: ES_EN=2, GS_EN, PRIMGEN_EN,
    GS_W32_EN, and GS_FAST_LAUNCH=1. Only the fast-launch field is outside what the geometry path accepts. Fast launch
    starts NGG subgroups without vertex inputs: the launch mode of mesh-shader-like work (radv used mode 1 for mesh
    shaders on GFX10.3, as far as I know; not checked here). The draws are `DRAW_INDIRECT_MULTI`, so these are probably
    Wolverine's mesh-style geometry, which the existing geometry path cannot take as is: it assembles input vertices
    per subgroup. Support would mean:
    - launching `vkCmdDrawMeshTasksIndirect` with the draw's count as the subgroup count;
    - giving the shader its subgroup id where fast launch puts it (which SGPR / VGPR is to be read from a dump).

    Not attempted. The rejection now names the fields (`43313d83`). For the day: dump one of these draws' shaders
    (`APS5_DUMP_SHADERS=1`) and note GE_NGG_SUBGRP_CNTL (uconfig 0x25b), the indirect record's vertex count and what
    the GS reads from its first VGPRs.
  - *Structurizer merge (1).* `conditional block 5 has no structured merge` (`ControlFlow/src/Structurizer.cpp`) on a
    `DRAW_INDIRECT` to color target 0x113f230000. The draw's shaders are not among the dumped requests (all 163 are
    compute), so the control flow cannot be looked at here. For the day: `APS5_DUMP_SHADERS=1` at the menu, and send
    the request of the shader that fails (the skip line names the draw; its programs are in the dump).
- **C6, duplicate GPU memory.** (b) and (d) implemented behind switches; (a) and (c) analysed, one diagnostic added.
  - *(a) Storage images duplicating a key.* The storage cache already keys on the decoded surface (`SurfaceKey`:
    address, extent, depth/array, mips, tiling, dimension, base array, storage format), not the raw T# words. So a
    duplicate the `[texmem]` report counts by (address, extent, format) is one of two things: a key field the report
    does not show, or an evicted image something still holds (a cached resource build, a recipe, a recorded batch).
  - *(c) Several extents per render target.* A writer's lookup already evicts the other extents at its address
    (`AliasEvictionEnabled`). Extents that stay alive are again held by something other than the cache.
  - *The diagnostic.* `b8000908`: with `APS5_TRACE_TEXTURE_MEMORY=1` the report ends with
    `[texmem] storage images alive: N in the cache, M evicted but still held; extra images ... K cached, L evicted`.
    A large M / L means the fix is in what holds evicted images (the resource cache's builds), not in the keys.
  - *(b) A snapshot beside a storage image of the same shape.* A sampled texture views the surface's storage image
    only when the surface is in host-imported memory (`SampledFromStorageEligible`). Elsewhere it is a CPU snapshot
    even when a storage image of that surface is already cached. Proposed fix: under a switch, view an already-cached
    storage image of the same `SurfaceKey` there too. Its `Refresh` keeps it current with guest memory, as for
    storage use.
    - *Tried tonight, not committed.* The fix itself was a dozen lines in `cachedTexture`. The test could not show it.
    - In the test harness, registered memory is host-imported, so the existing path already views it.
    - With plain host memory (a storage `image_load`, then a sampled read of the same surface), the second dispatch
      never reached the sampled cache's lookup: the dispatch-level caches served its resources. Counting snapshots
      there proves nothing.
    - A test needs a surface outside the imports whose sampled lookup really runs: for example a different shader, or
      the resource cache disabled for the test.
    - *Done later in the night* (`26a362bb`, `APS5_SAMPLED_FROM_CACHED_STORAGE=1`). The test turns host imports off
      (`APS5_NO_HOST_IMPORT`), so registered memory keeps its write-back but is not imported. It also turns the
      dispatch-level caches off (`APS5_NO_RESOURCE_CACHE`, `APS5_NO_DISPATCH_CACHE`) and samples with a second kernel.
      The stored results are written back before the sample, so the image is cached with nothing pending: a pending
      image is already viewed through `FindPending` without the switch.
    - With the switch, a lookup outside the imports first asks whether the surface's `SurfaceKey` has a cached storage
      image (`storageCached`, no image is made). If it does, and the DCC keys are uncompressed, the sample views it.
      Everything after that is the existing view path (the `UnchangedSince` check and `Refresh`).
  - *(d) Snapshot hashes: implemented behind `APS5_SNAPSHOT_DIGEST=1` (`29c7eeec`).*
    - **What the bytes were for.** `CachedTexture::bytes` (`ShaderResources.cpp`) is the CPU copy that
      `ReadTextureSurface` made for the upload. The upload copies it into a staging buffer at once. After that it
      serves one purpose: when the write watch cannot prove the surface unchanged (`UnchangedSince`), a lookup
      compares guest memory with it (`GuestMemory::EqualsCommitted`). The shared surfaces (`cache.surfaces`) work the
      same way. Fast-cleared entries are never compared (their validity is the keys).
    - **Change.** Entries hold a `SnapshotContents` (the size, and the bytes or their digest). With the switch, right
      after the upload the bytes become `GuestMemory::DigestBytes`: one 64-bit hash per 4 KiB chunk (four
      multiply-xor-shift lanes), 8 bytes per 4 KiB.
      - The comparison becomes `MatchesDigestCommitted`: after `FlushGpuWrites`, each committed chunk is hashed and
        compared.
      - Uncommitted chunks are skipped, as `EqualsCommitted` skips them. A chunk only partly committed counts as
        different (a new snapshot, never a stale texture).
      - `[texmem]`'s "CPU snapshots" figure counts the digests' size.
    - **Cost.** Measured in the container: the hash runs at 4.5-4.8 GB/s against memcmp's 7.6-8.1 GB/s on the same
      data (an 8-lane variant without the per-step shift gets 5.5 GB/s, but its mixing is weaker; not taken). So a
      comparison costs ~1.6x its CPU time, paid only where the write watch cannot prove a surface unchanged.
      Against that, 5-7 GB of RAM becomes ~12 MB. The odds of a false "equal" are ~2^-64 per changed chunk.
    - **Tests.**
      - `agc_driver_sampled_snapshot` (new, lavapipe) is the first test of the sampled snapshot path. A compute
        kernel `image_sample_lz`s a 32x1 R32_FLOAT texture in plain host memory three times. Every lane reads its
        texel. After a CPU write to one texel the next dispatch reads the new value and makes a new snapshot; with
        nothing changed the third makes none (`SampledSnapshotsCreated`).
      - `agc_driver_sampled_snapshot_digest` runs it with the switch. A digest that always matches makes it fail (lane
        5 reads the old texel).
      - The full suite passes with and without the switch, except the three known lavapipe failures.

- **All switches together.** The full suite with every switch of tonight and the day's on at once
  (`APS5_SUBROUTINE_CALLS APS5_SRT_PHI_SELECT APS5_IGNORE_LINEAR_ATTR_CLIP APS5_BINDLESS_STORAGE
  APS5_IGNORE_CONSERVATIVE_Z APS5_IGNORE_POPS APS5_DEPTH_CLEAR_DRAWS APS5_SNAPSHOT_DIGEST APS5_TRACE_TEXTURE_MEMORY`)
  fails only the three known lavapipe tests (`gds_append`, `sdwa_compare_scan`, `vopc_compare`).
  - `agc_driver_flip` failed once in a `-j8` suite run after `cd49c891`, and passed alone and in the next full run.
    It waits on 5 s timeouts, so it is load-sensitive, not related to the change.
  - The 163 dumped menu compute requests through `agc_shader_replay`: 38 compile and 125 fail, both without
    switches and with `APS5_SUBROUTINE_CALLS=1 APS5_SRT_PHI_SELECT=1 APS5_BINDLESS_STORAGE=1`, and it is the same
    125 files. With the switches, the failures move further along:
    - `11d0395500` gets past "bindless storage image tables are unsupported" to the SRT evaluation;
    - `7ff6bd996c00` / `7ff6bd9d7100` get past "subroutine calls are off" to "the code of callee 0x11d039d800 /
      0x11d039d400 is not in the request".

    Of the failures, 104 are `ReadConst(GetSrtResource...)` in the SRT evaluation: the SRT memory is not in the
    dumped requests. So these dumps cannot show the switches working; a re-dump on the day machine does (item 2).

### For the day session (RTX 5080), in order

1. Menu run with the day switches plus tonight's: `APS5_SUBROUTINE_CALLS=1 APS5_SRT_PHI_SELECT=1
   APS5_IGNORE_LINEAR_ATTR_CLIP=1 APS5_BINDLESS_STORAGE=1 APS5_IGNORE_CONSERVATIVE_Z=1 APS5_DEPTH_CLEAR_DRAWS=1
   APS5_SNAPSHOT_DIGEST=1 APS5_SAMPLED_FROM_CACHED_STORAGE=1 APS5_TRACE_TEXTURE_MEMORY=1 APS5_PS_STATE_FROM_SHADER=1`. If something breaks, add them one at a time.
   - Expected skips gone:
     - the `skipped dispatch` lines for `7ff6bd996c00`, `7ff6bd9d7100`, `7ff6bd9d4400`, `7ff6bd9da500`, `11d040e200`
       and `11d0395500`;
     - the 7 `PA_CL_CLIP_CNTL` draws;
     - the DB_RENDER_CONTROL draws (4) and the conservative-Z draws (2);
     - the 6 interpolation draws (`APS5_PS_STATE_FROM_SHADER=1`; each prints the `[gpu] pixel shader 0x11d015f100` line).
   - `APS5_SAMPLED_FROM_CACHED_STORAGE=1` should also lower the `[texmem]` "duplicate" counts for surfaces that are
     both stored and sampled (C6 (b)); if the menu shows stale or flickering textures, run without it first.
   - Expected memory: the `[texmem] sampled cache` line's "CPU snapshots" falls from GB to MB, and process RAM by as
     much. Check that the menu still renders: a wrong digest would show stale textures.
   - Copy the `[texmem] storage images alive` lines at the menu (C6: cached vs evicted-but-held).
   - A remaining call skip names its callee: say whether that address is registered shader code (the driver only
     adds registered code).
   - A regression would show as new `skipped` lines, or wrong pixels on the menu's UI layers (the clip flag) or
     where depth is cleared.
   - Separately, `APS5_IGNORE_POPS=1` (the 2 raster-ordered draws): look for ordering artifacts where the menu blends
     overlapping layers.
2. With `APS5_DUMP_SHADERS=1`, re-dump the two call shaders: the new requests carry the callee regions and should
   compile with `agc_shader_replay` (with `APS5_SUBROUTINE_CALLS=1`).
3. For the 6 interpolation draws: log `SPI_PS_IN_CONTROL` (context 0x1b6) at those draws, and where the game sets it
   (a `SET_CONTEXT_REG` of 0x1b6, or the PS header's registers). If it is never written, the PS header's
   `spi_ps_in_control` is not being applied.
4. Run on Windows: `agc_driver_subroutine_calls`, `agc_driver_flat_apertures`, `agc_srt_phi_select*`,
   `agc_driver_bindless_storage`, `agc_driver_depth_clear_draw`, `agc_driver_sampled_snapshot*`,
   `agc_driver_sampled_from_cached_storage*` and
   `agc_driver_graphics*` (it includes `_db_shader_hints` and `_depth_clear_draws`).
5. With `APS5_DUMP_SHADERS=1`, the requests of the fast-launch draws (C5, 0x482030) and of the draw that fails in
   the structurizer (`conditional block 5`).
6. The `skipped draw: INDEX_BUFFER_SIZE has not been set (...)` line: copy the text in the parentheses (C5 above
   says what each case means).
7. If `11d0395500` runs: check that whatever it writes (its output image, sampled later by the menu) looks right. A
   wrong key would write another table entry's image.

## Day 2026-10-07 (RTX 5080, live runs): from the post-intro abort to the menu load

Run switches: `APS5_RESERVE_HONOR_HINT=1 APS5_AMM_RANGE_GIB=11 APS5_IMPORT_WINDOW_SPLIT_REMAINDER=1
APS5_TOLERATE_DISPATCH=1 APS5_NO_SNAPSHOT_CHECK=1`. Intros ~20-25 fps; the game then loads the main menu
(frame ~850-890) and stalls there on GPU memory.

Fixed (commits on `wolverine-next`):
- **Post-intro abort** (wait on a null address): the game's streaming heap (`eboot+0x3d82310`) allocates over the
  AMM range and returns its slack (`0x4000`/`0x8000`) as the pointer when the range is out of address space.
  `APS5_AMM_RANGE_GIB=11` (AMM ranges must stay below `0x1000000000`; 32 GiB crashed) and
  `APS5_RESERVE_HONOR_HINT=1` (the game's own 8 GiB pool asks for hint `0x2680000000`; placed elsewhere it blocks
  the AMM range's growth). `82aa9070` and earlier.
- **2.7 GiB copy at the menu load** (`d32ef2e6`): a plain buffer descriptor binds the whole streaming heap
  `0x1210000000+0xab000000`; no cell import covered it and the budget refused one, so it fell back to a 2.7 GiB
  host buffer that heap 1 never holds. Now the range's heap mirror chunks are dropped, the recorder synced, and the
  range imported whole (`[gpu] whole import ... ok`). `APS5_NO_WHOLE_RANGE_IMPORT=1` keeps the copy.
- **Compute scratch** (`9a5734df`): `COMPUTE_PGM_RSRC2.SCRATCH_EN` dispatches run; the recompiler sizes scratch from
  the AGC header's `scratch_size_dw_per_thread` (byte 84).
- `APS5_TOLERATE_DISPATCH=1` skips dispatches whose shader fails to compile (not driver errors).
- Multichannel Opus gets an OpusHead (FFmpeg needs it for more than 2 channels).

GPU memory at the menu load (`[texmem]` under `APS5_PROFILE_DRAW`, `85981ad3`; `APS5_TRACE_TEXTURE_MEMORY`):
- the menu's bindless table references ~3250 surfaces, ~8 GB of distinct textures, in **one build** (the sampled
  cache cannot evict them and goes over its budget);
- host images are not larger than guest (no format growth);
- textures + storage images: 4209 host images, 11.5 GB, over 9.45 GB of guest (x1.21). Duplicates: storage images
  at one address with several extents (2560x1440, 3328x1872, 3840x2160); a snapshot texture beside 1-2 storage
  images of the same shape; 261 storage images duplicating an (address, extent, format) key;
- heap 1 (host-visible, 15.4 GB budget) overflows first: imports 10.8 GB + host buffers 1.4 + shadow slabs 0.3 =
  12.5 GB tracked, ~3 GB untracked;
- the game uses 26-32 GB of the PC's 32 GB RAM (CPU snapshots of sampled textures alone 5-7 GB);
- tried without effect: `APS5_RESOURCE_CACHE_PINNED_MIB=2048` (VRAM -0.3 GB, intros 7 fps),
  `APS5_TEXTURE_CACHE_MIB=6144` (more churn, same OOM).

## Night 2026-10-07

Base: `0c20c3b3` (`wolverine-next`). Built in `build-wolverine` on lavapipe. The full ctest suite (154 tests) passes
except `agc_driver_gds_append`, `agc_driver_sdwa_compare_scan` and `agc_driver_vopc_compare`, which fail at the base
commit too (checked with tonight's changes stashed). `gds_append` and `sdwa_compare_scan` first differ at lane 8,
and lavapipe's subgroup size is 8, so those two are probably lavapipe artifacts. `vopc_compare` differs at thread 1
(compare 39); it fails on `night/perf` and `gta-v/hw-rt` too. None was checked on a real GPU.

Build fix carried over from `gta-v/hw-rt`: `ShaderDiskCache.cpp`'s Linux layout asserts now match this branch's
structs (`ImageResource` 64 bytes here, `ResourceSpecialization::Image` 44, measured with a probe; the encoders were
checked field by field). `night/wolverine` builds on Linux as checked out.

Test infrastructure, later in the night: the recorder tests (`agc_driver_recorder_tests`) stopped at
`unitShadowTests` on lavapipe, which shows a GPU store into a host import to the write watch. The fix from
`gta-v/hw-rt` is cherry-picked: those parts skip there (`GpuStoresLookLikeCpuStores`), and the binary is now the
ctest `agc_driver_recorder`, which passes.

### Blocked, and why

- **C3b** (`BeginHostWrite` rejecting holes): the code is Windows-only (`WindowsMappings`, section views and
  `VirtualProtect`). On Linux `GuestArenaBeginHostWrite` returns true, so a change could be neither built nor tested
  here; analysis below.
- **C3c**, second half: the null page in `ShaderMemory::read` still answers 0. The guards (`APS5_SRT_GUARDS`, below)
  remove the null reads they can see. Only Wolverine shows whether any remain, so failing the null page is left
  for the day session.
- **Game-side checks** of C1-C3a: need Wolverine running.

### For the day session

1. Build `night/wolverine` and run the intros with `APS5_PROFILE_DRAW=1`. Check that the indirect-dispatch CPU reads
   are near zero (C1, on by default since `bf5c5766`).
2. `APS5_TRACE_AJM=1`: note any resample ratios Wolverine sets. If they are not 1, listen with `APS5_AJM_RESAMPLE=1`
   (C2); if the speed sounds inverted, the ratio direction needs flipping.
3. `APS5_AMM_POOL=1 APS5_TRACE_APR=1` (C3a): check the given memory goes to the pool and the maps succeed. Compare
   direct memory use against a run without the switch.
4. C3b: on Windows, find the call that fails. Log `BeginHostWrite`'s false returns with the range and the first
   non-writable view, then decide whether a range with a read-only view should open its writable pages instead.
5. C3c: run the intros with `APS5_SRT_DEBUG=1`, without and then with `APS5_SRT_GUARDS=1`. Every null-page read
   prints `[srt] null-page read 0x... (shader 0x...) while evaluating ...`, naming a flat slot, a descriptor source
   dword or a branch condition. Each item the guards skip prints `[srt] guards skip flat slot N` or `[srt] guards skip descriptor source N`. Expected with the
   switch: the flat-slot null reads are gone and the intros look and sound the same.
   - Descriptor sources are guarded too (`feat(shader): the SRT guards also cover descriptor sources`), so null
     reads should remain only where a branch condition cannot be evaluated or a source is shared with code outside
     the guard.
   - If no null reads remain, make the `address < PageBytes` branch of `ShaderMemory::read` fail the read, under a
     second switch.

### Commits

| Task | Commit | Switch | What the test proves |
|---|---|---|---|
| C1 thread-dimension indirect dispatches | `test(agc): thread-dimension indirect dispatches on the GPU and the CPU path` | already on this branch (`bf5c5766`, on by default; `APS5_CPU_THREAD_DIMENSIONS=1` restores the CPU read) | `agc_driver_thread_indirect_dispatch` (GPU pass) and `agc_driver_thread_indirect_dispatch_cpu` (CPU read): for 1, 40, 63 and 64 threads, an indirect dispatch writes exactly what the direct thread-dimension dispatch of that count writes; lanes past a partial count write nothing; on the GPU path no argument is read on the CPU (`cpuReason == 0`) |
| C2 AJM resampling | `feat(ajm): resample the decode output by the instance's ratio` | `APS5_AJM_RESAMPLE=1` | `ajm_resampler`: ratio 1 passes PCM through, 2 takes every other frame, 0.5 adds linear midpoints; chunked streams match whole ones; the per-sample ratio change applies; s16/s32 round and clamp; output stays within the room bound |
| C3c SRT branch guards | `feat(shader): the SRT walk skips flat slots behind branches it can rule out` | `APS5_SRT_GUARDS=1` | `agc_srt_guards` and `agc_srt_guards_on` (one binary, without and with the switch), on a hand-assembled shader that loads a value through a pointer only when a flag is set. The walk reads memory through a reader that fails on the null page. With the switch, a zero flag skips the value's slot (0), the null page is never read, and a set flag still puts the value in the table. (Since the next commit the store's V# is skipped too, so the test counts two skips.) Without it, the plan has no guard blocks and a zero flag with a null pointer reads the null page. The whole suite passes with the switch on and off (the three baseline failures aside) |
| C3c guarded descriptor sources | `feat(shader): the SRT guards also cover descriptor sources` | `APS5_SRT_GUARDS=1` (the same switch) | `agc_srt_guards` / `_on` add a shader that loads a V# through the guarded pointer and stores through it. With the switch, a zero flag leaves that source as a null descriptor, skips its four flat slots, and never reads the null page. Without it, the walk reads the null page. In the first shader the store's user-data V# is now skipped as well (used only behind the guard). The suite passes with the switch on and off (baseline failures aside) |
| C3c null-read trace | `feat(shader): APS5_SRT_DEBUG names what the walk evaluated at a null-page read` | none (debug output under the existing `APS5_SRT_DEBUG`) | `agc_srt_guards_trace`: the unguarded walk of the guard test's shader prints its null-page read as the value's flat slot (slot 1) |
| C3a AMM donated pool | `feat(kernel): the AMM's donated direct memory as a backing pool` | `APS5_AMM_POOL=1` | `guest_amm_pool`: a map anywhere in the AMM range is backed by the given memory (read through a CPU alias of it), lowest piece first; a map larger than the pool's free pages takes them and fresh memory for the rest; an unmap returns its pages. `guest_memory` (the prefix mode) still passes |

### C1: thread-dimension indirect dispatches

The GPU pass the queue asks for already exists on this branch (`bf5c5766`, Oct 3): `ThreadGroups.comp` converts the
thread counts to group counts for `vkCmdDispatchIndirect`, and a copy puts them in the shader's runtime thread limit.
It is on by default; `APS5_CPU_THREAD_DIMENSIONS=1` restores the CPU read.

What was missing is a driver test on lavapipe, which is now added. Both paths are compared against the direct
thread-dimension dispatch of the same thread count. The kernel mixes values across lanes, so a partial wave's values
legitimately differ from a full one's; the direct partial dispatch is the oracle.

Day-session check: with `APS5_PROFILE_DRAW=1` in Wolverine's intros, the indirect-dispatch CPU read count
(`IndirectOutcome::cpuReason` != 0) should be near zero. A thread-dimension dispatch whose arguments are not in a
host import (reason 3) or are under a pending label (reason 2) still falls back to the CPU.

### C2: AJM resampling (`APS5_AJM_RESAMPLE=1`)

`sceAjmBatchJobSetResampleParameters(Ex)` recorded the ratio, and nothing used it. With the switch on, the decode
output of AT9, MP3 and Opus instances goes through a per-instance linear resampler (`libSceAjm.native/src/Resampler.hpp`)
while the ratio is not 1 or a ratio change is set:

- **Order:** applied after gapless trimming; `totalDecodedSamples` still counts decoded frames.
- **Encodings:** s16, s32 and f32, with the same rounding and clamping as the decoders' conversions.
- **Continuity:** the fractional position and the last input frame carry across runs, so consecutive decode jobs
  resample as one stream. `ClearContext` and `Initialize` reset them.
- **Ratio change:** applied after every output frame (the Ex variant's `ratio_change_per_sample`). The ratio is
  floored at 1/64, and `GetResampleInfo` reports the current ratio.
- **Room:** each run's room check uses the resampled size (`MaxOutputFrames`), so `NOT_ENOUGH_ROOM` is reported as
  before. Opus's overflow buffer holds resampled PCM.

Unverified, and marked as debt in the header: the ratio's direction (taken as input frames per output frame: 2 plays
twice as fast) and the console's filter (linear here).

Day-session check: find what Wolverine sets (`APS5_TRACE_AJM=1` prints `[ajm] instance N resample ratio R change C`).
If it sets ratios other than 1, compare the audio with and without `APS5_AJM_RESAMPLE=1`: pitch and speed should
match what the ratio implies. If it sounds inverted (2 plays slower), the direction is the other way: swap it in
`Resampled()`.

### C3a: the AMM's donated memory as a backing pool (`APS5_AMM_POOL=1`)

Before, `AprAmmGivePhysical` mapped the donated direct memory at the start of the AMM's 8 GiB range (an auto-mapped
prefix). Maps inside the prefix reused those fixed pages, and maps elsewhere allocated direct memory of their own, so
a title mapping its VA wherever it likes used the donation only by chance and allocated more besides.

With the switch, the donation goes to the AMM's free physical pool and nothing is mapped up front. Every map takes the
pool's pieces lowest first, and as many as it needs, because a fragmented pool backs a map in several pieces. It
allocates direct memory only for what the pool cannot cover. An unmap returns its pages to the pool (fresh pages too,
as before).

Day-session check: run Wolverine with `APS5_AMM_POOL=1 APS5_TRACE_APR=1`. Expected: `[amm] given ... to the pool`,
then maps without `out of direct memory`. The direct memory in use should stay at about the given size plus what the
title maps beyond it.

### C3b: `BeginHostWrite` and holes (analysis, no change)

`WindowsMappings::BeginHostWrite` walks the shared views overlapping the range, and the outcome depends on what it
finds:

- It returns false if any of them is not writable, and then opens none of them.
- Pages with no view at all (holes: private memory, or nothing) are skipped silently.

So a host write over a range mixing a read-only shared view with writable ones is refused as a whole. The caller
(`sceFiberInitialize`'s context fill, the `HostWrite` scope) then writes through armed views and takes the
write-watch faults, which `HandleWrite` resumes only for writable views. A context over a hole followed by shared
pages is opened only for the shared part, which is correct.

What to decide on Windows:

- whether "rejecting holes" in the bring-up note means a hole between shared views of one context (a mapping the
  game made in pieces);
- or whether it means the all-or-nothing refusal. If so, the change would be: open the writable views, return which
  part was opened, and leave read-only views alone.

Behind a switch it would be a few lines in `BeginHostWrite`/`EndHostWrite` (both must agree on the views they
counted), with a Windows test in `GuestMemory.cpp`.

### C3c: the SRT walk's branch guards (`APS5_SRT_GUARDS=1`)

`ShaderMemory::read` answers any read below 4 KiB with zero. The reason: the SRT walk reads through pointers that
the shader dereferences only behind a branch, and the walk evaluated every flat slot (`srtReads`) whatever the
branches.

The walker's branch logic (`controlFlow`, with per-block conditions, successors and descriptor sources) already
existed, but nothing filled `controlFlow`, so it never ran. The earlier note here was wrong on two counts. It said
the walker respected branch guards for descriptor sources; it did not. It said a block index on `SrtRead` needed a
disk-cache format bump; it does not, because the disk cache stores compiled variants and not the plan (the plan is
rebuilt from the code on first use).

With the switch:

- `ResourceMaterializer::ExtractPlan` calls `BuildSrtGuards`. It builds one `ResourceBlock` per IR block from the
  terminators (`trueBlock` first, as `OpBranchConditional` takes it). It notes each slot's blocks from the
  program's `ReadConst(GetSrtResource, slot)` instructions, which are exactly where the shader loads the flattened
  table. A condition whose cone exceeds 1024 values is left unevaluated (both successors). Any unsupported
  terminator, or a slot read with a dynamic index, leaves the plan without guards.
- The walk (`EvaluateRuntimeSourcesImpl`) marks the blocks reachable from the entry. It follows a conditional
  branch one way when the condition evaluates on the runtime memory, so the condition's reads are captured like
  any other and a change to them forces a new capture. It skips (leaves 0) a slot all of whose blocks are
  unreached. Those slots are never loaded by the shader.
- Descriptor sources, in a second commit: `ResourceTracker` notes, per source, the blocks of the instructions that
  use it (`IrResourcePlan::sourceBlocks`). `BuildSrtGuards` lists each source in those blocks. The walk makes a
  listed source active only when one of its blocks is reached, and an inactive source reads as a null descriptor
  (zeros, which the materializer already handles as a null V#/T#). Sources no instruction named (an indirect
  image's table and material buffers) stay active. A source shared with a reached block stays active.
- Slots read in a condition are impure (`ComputePureFlatSlots` marks condition cones), so the draw cache never
  patches them as data words.

Why this is safe: a block the walk did not reach cannot run with the same memory, because the walk only cuts an
edge whose condition it computed from that memory. A value is read in its defining block, which dominates its uses,
so a used slot's block is always reached. Stale memory (a GPU write after the capture) affects the flattened table
the same way as before.

Expected effect: fewer null-page reads and fewer reads overall in the walks of guarded shaders. No change to the
SPIR-V or the variant keys.

Day-session check: see step 5 above.
