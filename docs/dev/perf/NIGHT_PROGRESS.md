# night/perf progress

## Night 2026-10-07

Base: `a75aea29` (`gta-v/fidelity-main` + the pipelined-draws work). Built in `build-perf` on lavapipe; the
`agc_*` ctest suite (143 tests) passes with the switches below off and on, except `agc_driver_vopc_compare` and
`agc_driver_vopc_compare_table`, which fail at the base commit too (lavapipe).

### Commits

| Task | Commit | Switch | What the test proves |
|---|---|---|---|
| B7 exit crash | see `git log` (`fix(agc): retire the buffer pool ...`) | none (bug fix) | `agc_driver_buffer_pool_retire`: a retired pool frees its retained slots while the device lives and makes no Vulkan call for anything returned later, including a `Buffer` destroyed after the teardown (the image-mirror case) |

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
