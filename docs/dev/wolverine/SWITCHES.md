# Switches: from a game workaround to a pull request

`night/merge` is the one development branch every game runs on. New or unproven behaviour goes in behind an `APS5_*`
switch that is off by default, so a Wolverine change cannot break GTA V. A switch is never a pull request by itself:
upstream wants exact behaviour or a clear refusal, not environment toggles. Every switch is one of three kinds, and
the kind says what the pull request will be.

| Kind | What it is | Path to a pull request |
|---|---|---|
| **Fix** | Hardware-accurate behaviour, off only until it is checked on the other games. | Check it on GTA V (replay `gta-perf-3` and one live run) and the other games, make it the default, delete the switch, then open the PR with its test. |
| **Workaround** | Skips or approximates something not implemented (a refusal turned into a guess). | Never a PR. It marks missing behaviour; the PR is the real implementation, and the switch is deleted with it. |
| **Stand-in** | A Wolverine-specific setting for kernel behaviour we have not modelled right. | Never a PR. The PR is the correct kernel behaviour, which makes the setting unnecessary. |
| **Diagnostic** | Traces and debug aids. | Stays off; upstream takes only the ones that are generally useful. |

## The switches Wolverine runs with

| Switch | Kind | Where | What it does | Left before a PR |
|---|---|---|---|---|
| `APS5_WIDE_BINDLESS` | Fix | recompiler `ResourceMaterializer`, runtime ABI 14 | Bindless image tables of thousands of entries bound as per-dimension descriptor arrays with a key map; only sampled entries are bound (feedback buffer). | GTA V check (its tables are small: must render the same); a design note for upstream (runtime ABI change); then default on. `APS5_NO_WIDE_FEEDBACK` goes with it. |
| `APS5_COMPACT_BDA_HELPERS` | Fix (exact, size) | `SpirvBdaRead.cpp`, `SpirvBdaLookup.cpp` | The dword byte path loops around one lookup call; a lookup reports its checks through one fault site (first failing reason; the stages break to it). Main's span read loops its per-dword fallback. Same faults and values, ~4x fewer inlined words. | GTA V replay (same frames), the `_compact` ctests on the 5080, then default on. |
| `APS5_BACKGROUND_PIPELINE_WORDS` | Workaround | `VulkanDevice.cpp` | A compute pipeline of at least n SPIR-V words (Wolverine: 1000000, for the material lighting 0x11d049aa00, ~5.5 min and 6.6 GB on NVIDIA) is created on its own thread; the program's dispatches until then record their resources and barriers but bind and dispatch nothing (logged). Replaces `APS5_SKIP_PROGRAMS=11d049aa00`. | Never a PR: the PR is a program NVIDIA compiles in seconds (fewer inlined guarded reads, `agc_dispatch_run --compile-only` to measure). |
| `APS5_FORMAT_SHARED_READS` | Fix (exact, size) | `SpirvRuntimeBuffer.cpp` | Runtime-format loads read each component position once for every layout. | Same as above, with `agc_driver_buffer_format_compact`. |
| `APS5_BINDLESS_STORAGE_TABLES` | Fix | `ResourceTracker.cpp`, `ResourceMaterializer.cpp`, `SpirvImageEmitter.cpp` | Image stores and atomics through a bindless table: the table's entries are bound as storage heap slots (also under `APS5_WIDE_BINDLESS`), the slot indexes the store. | 0x11d0395500 live (capacity), a GPU-time look at the write-backs of every bound slot, GTA V replay, then default on. |
| `APS5_STENCIL_EXPORT` | Fix | `State.cpp` (the recompiler always honours the flag) | DB_SHADER_CONTROL STENCIL_TEST_VAL_EXPORT_ENABLE with a 32_GR or 32_ABGR Z format: MRTZ component 1 to FragStencilRefEXT (`VK_EXT_shader_stencil_export`). | NVIDIA has no `VK_EXT_shader_stencil_export` (checked 2026-10-11 on the RTX 5080: the draw into 0x113f230000 then fails to compile, "needs VK_EXT_shader_stencil_export, which the device lacks"), so it needs an AMD or Intel device for the check, or an emulation of the export for NVIDIA; then a GTA V replay and default on. |
| `APS5_DEPTH_CLEAR_DRAWS` | Fix | `State.cpp`, `Draw.cpp` | DB_RENDER_CONTROL depth/stencil clear draws recorded as attachment clears. | GTA V check, default on, PR with `agc_driver_depth_clear_draw`. |
| `APS5_PS_STATE_FROM_SHADER` | Fix (to confirm) | `DrawDecode.cpp` | A pixel shader header's context registers used when the draw's context lacks the interpolants. | Confirm on hardware documentation that the header registers apply; a test from a menu draw; then default on. |
| `APS5_EAGER_LINEAR_WRITEBACK` | Fix (perf trade) | `Texture.cpp` | Linear storage results written back at once (CPU loads are not hooked, so a deferred write-back is never seen). | Replace by a write-back the CPU read triggers; or default on if GTA V does not slow down. |
| `APS5_DRAW_SNAPSHOT_MAX_MIB` (default 256) | Fix | `ShaderResources.cpp` | Largest read-only buffer a draw copies into a snapshot. Wolverine runs with 32. | Pick one default that suits both games (measure GTA V at 32 and 64). |
| `APS5_IGNORE_CONSERVATIVE_Z` | Fix (exact) | `State.cpp` | Accepts DB_SHADER_CONTROL CONSERVATIVE_Z_EXPORT, a hint. | Default on: running without the hint is exact. |
| `APS5_IGNORE_POPS` | Workaround | `State.cpp` | Accepts PRIMITIVE_ORDERED_PIXEL_SHADER without raster ordering. | Real implementation: fragment shader interlock (`VK_EXT_fragment_shader_interlock`). |
| `APS5_NULL_UNRESOLVED_IMAGES` | Workaround | `ResourceTracker.cpp` | Images whose descriptor the GPU selects at run time read as null. | Real implementation: GPU-selected descriptors (runtime descriptor heap). |
| `APS5_NO_SNAPSHOT_CHECK` | Workaround | `GuestBufferMemory.cpp` | Skips the guest-snapshot-differs check at the menu load. | Find why the snapshot differs; fix that; delete. |
| `APS5_AMM_RANGE_GIB` (11) | Stand-in | `Apr.cpp` | AMM range size; Wolverine packs AMM addresses into 36 bits. | Model the PS5 AMM address layout so the default fits. |
| `APS5_RESERVE_HONOR_HINT` | Stand-in | `DirectMemory.cpp` | Reservations honour the address hint (the game's 8 GiB pool). | Check the SDK semantics; if hardware honours hints, default on and delete. |
| `APS5_HEAP_MIRROR_HOST` | Escape hatch | `GuestBufferMemory.cpp` | Restores the old heap mirrors in host memory (the new default is ReBAR device memory). | Delete once GTA V is checked on the new default. |
| `APS5_KEEP_REPLACED_TEXTURES` | Escape hatch | `VulkanDevice.cpp` | Keeps a replaced device's texture caches (the new default clears them). | Delete once checked. |
| `APS5_TRACE_GPU_MEMORY`, `APS5_GPU_CHECKPOINTS`, `APS5_LOOP_GUARD`, `APS5_WATCH_MEMORY` + `APS5_WATCH_PRINT` | Diagnostic | driver, recompiler | Memory line, device-loss checkpoints (also dispatch ends and draws), loop iteration guard, guest memory after each dispatch (with `APS5_SYNC_DISPATCH=1`). Wolverine no longer needs the loop guard (9978fb57a fixed the menu spin). | Stay. |

Run-script settings that are not switches of their own: `APS5_NO_RESULT_MEMO=1`, `APS5_DISPATCH_CACHE_ENTRIES=64` and
`APS5_RESOURCE_CACHE_ENTRIES=128` keep Wolverine's RAM flat while the caches hold per-entry copies of the wide tables
(PORT_INVENTORY / NIGHT_SHIFT W1: bound the caches by bytes instead).

## Changes made default-on for every game on `night/wolverine-merge` (to check on GTA V before the fold into `night/merge`)

APR hybrid execution, the structurizer's relaxed join, DeepStack, heap mirrors in ReBAR, whole-range pinned imports,
the draw snapshot cap, aperture registers, fast-launch point-per-subgroup, GPU thread-group conversion, the runtime
ABI 14 binding numbers, pixel inputs read their default, the GDS M0 base, the device heap fallback, clearing a replaced
device's textures, and the wide table memo and class memo.
