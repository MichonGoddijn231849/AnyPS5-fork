# Frame capture and replay

Capture what the AGC driver consumes for a few frames of a running title, then replay it through the same driver without the game.

## Capture

Set two environment variables before starting the title:

| Variable | Meaning |
|---|---|
| `APS5_CAPTURE=<dir>` | Output directory (created). |
| `APS5_CAPTURE_FRAMES=a-b` | Capture starts after flip `a` was submitted and ends after flip `b`; `a-b` records `b - a + 1` frames. Default `60-62`. |
| `APS5_CAPTURE_MAX_GB=<n>` | Abort the capture (not the game) when the files pass `n` GiB. Default 20. |

At flip `a` the submitting thread waits until every earlier submission completed (3 s; otherwise it retries after the next flip), drains the device, and writes the base snapshot. The game keeps running after the capture finishes. Progress lines start with `[frame-capture]`.

Output:

- `capture.txt`: status, frames, submissions per queue, address-space size and changes, base/delta sizes per frame, timings.
- `events.bin`: the ordered event stream.
- `pages.bin`: every distinct non-zero 4 KiB page, stored once.
- `live/frame_NNNNN.png`: the frames the game presented, read back on the GPU.

## Replay

`build/core/libs/libs/unpatched/agc_frame_replay.exe <capture dir> [options]` (it loads the unpatched modules next to it).

| Option | Meaning |
|---|---|
| `--loop N` | Replay N times; between loops memory, mappings, queue state and GDS return to the capture's start. |
| `--png DIR` | Write the replayed frames of the first loop (`--png-all-loops` for all) as PNG; `--png-scale N` subsamples. |
| `--compare DIR` | After the replay, compare `--png` frames with DIR (default `<capture>/live` when `--png` is given): PSNR, mean and max difference per frame. |
| `--no-pacing` | Submit without waiting for the recorded per-queue packet progress. |
| `--settle` | Also drain the device before every memory delta. |
| `--shader-cache DIR` | Shader/pipeline disk cache (default: next to the exe); point it at the game's run dir cache for a warm first loop. |
| `--hidden` | Do not show the presentation window. |
| `--cold[=classes]` | Between loops clear the driver's in-memory caches so every loop misses them like new frames: `dispatch` (dispatch cache and its recipes), `draw` (draw cache and recipes), `resources` (resource cache), `textures` (sampled/storage textures, flushed first), `tables` (bindless sampled tables, BDA tables), `space` (address-space cache); `--cold` alone clears all. The shader disk cache, compiled shaders and pipelines stay warm. On Wolverine's intros `--cold=dispatch,draw,resources,tables,space` matches the game's per-dispatch costs best: the game keeps its textures resident across frames. |

All driver environment variables apply (`APS5_PROFILE_DRAW=1`, `APS5_PROFILE_GPU=1`, ...). Profile lines report every 10 s, so profile with enough loops. Exit code: 0 on success, 2 when commands differed from the capture or queues were left blocked, 1 on errors or missing/different frames in `--compare`.

## Format (version 2)

`events.bin` starts with `APS5CAP1` and a version word, followed by records `{u32 type, u32 0, u64 bytes, payload}`:

| Event | Payload |
|---|---|
| Begin | first/last frame, flips before the capture |
| AddressSpace | new backings, removed/added pieces, removed/added registry ranges (the first one is the whole space) |
| Memory | kind (base, delta, mapped), page runs, page indices into `pages.bin` (`0xffffffff` = zero page) |
| Progress | per queue: packets executed since the capture start, observed before the following delta |
| Submit | queue, flips, original packet address/size, flattened words and their hash |
| Suspend | a suspend point |
| Shader | code/header address, type, the driver's copies of code and header |
| QueueState | queue registers, constant RAM and draw state at the capture start |
| DriverState | the pending graphics reset flag and GDS |
| VideoOutput | handle registered/unregistered |
| Present | flip index, display buffer (or a blank flip) |
| End | flips and submissions recorded |

A piece is a mapped run of guest memory: private arena memory, direct memory (a backing id and offset, so aliases of one physical page stay aliases), or memory outside the arena (the main image).

## Limitations

- Looping the same frames makes every dispatch, draw and texture hit the driver's caches after loop 0, while live frames with new content miss them; `--cold` clears the caches between loops.
- Loop 0 starts with cold driver caches (the shader disk cache can be warm).
- Pacing waits for packets the workers executed, not for deferred labels or GPU completion; `--settle` is the strict variant.
- CPU writes to memory outside the write-watched arena (the main image's data sections) after the base snapshot are not recorded.
- Timing-dependent driver decisions (validation sampling, waits, label polling) can take other paths than in the game.
- Mapping changes are recorded at submission granularity; the replay maps and unmaps whole pieces at the recorded physical offsets.
- The replay process must be able to reserve the captured image range (it fails loudly if a DLL occupies it).
