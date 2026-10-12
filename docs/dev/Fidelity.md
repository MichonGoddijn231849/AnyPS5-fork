# Rendering fidelity harness (`tools/fidelity`)

Measures how much of a captured frame renders correctly, for any game with a frame capture
(`agc_frame_replay`). It reads only driver trace lines, so it has no game-specific logic.

```
python tools/fidelity/fidelity.py --capture <name> inventory --run       # traced replay -> inventory.json (+ live view)
python tools/fidelity/fidelity.py --capture <name> sweep --rounds N      # capture -> check -> prune, frame order, 1 replay/round
python tools/fidelity/fidelity.py --capture <name> report --live         # the scores, redrawn every second
python tools/fidelity/fidelity.py --capture <name> view next --ask       # next pass to judge, in the terminal, then a verdict
python tools/fidelity/fidelity.py --capture <name> view ID [--slice Z] [--inputs] [--mode auto|sixel|blocks|png]
python tools/fidelity/fidelity.py --capture <name> capture [--passes ID,...] [--limit N] [--budget-gib 6]
python tools/fidelity/fidelity.py --capture <name> check [--passes ...] [--ulps 4] [--prune]
python tools/fidelity/fidelity.py --capture <name> verdict ID ok|wrong|unknown [--note TEXT]
python tools/fidelity/fidelity.py --capture <name> finding ID "text"    # a lead, shown in the report and status.json
```

The work directory is `ps5run/captures/fidelity/<name>/`:

| file | written by | content |
|---|---|---|
| `config.json` | you | `capture`, `replay` (agc_frame_replay.exe), `args`, `env` (the game's switch set), `path`, `known_skips` (program -> why), `commit_gib` |
| `inventory.json` | inventory | every pass in frame order: id, kind, queue, submission, programs, draw serials, inputs, outputs (address, bytes, WxH[xN], format, tile), issues |
| `captured.json` | capture | per captured pass: its directory and kind |
| `passes/<id>/` | driver | `packets/`, `packets.txt`, `shaders/`, `before/`, `after/`, `ranges.txt`, `done.txt` (see below); `shader.req`, `dispatch.txt` for dispatches; `offline/` or `isolated/` from the check |
| `capture-passes.txt.blobs/` | driver | the range contents, one file per distinct content (pass directories hard-link them; chunk manifests name them) |
| `checks.jsonl` | check | one line per check: `match`, `near`, `mismatch`, `unstable`, `incomplete` or `error`, with details |
| `verdicts.json`, `findings.json` | verdict, finding | your ok / wrong per pass; leads per pass |
| `progress.json` | every phase | the running phase and its progress |
| `status.json` | every command | the scores below; **another session polls this file** (written atomically) |

Each replay runs in its own folder under the work directory (`run-inventory/`, `run-capture/`), because the driver
writes its dumps into the current directory. A replay refuses to start while another `agc_frame_replay` runs, or
with less than `commit_gib` of commit memory free. The capture keeps 5 GiB of the disk free.

## Passes

- A dispatch is one pass: `c:<program>#<n>`, where n counts the program's `[dispatch-io]` headers. That is the
  instance number `APS5_CAPTURE_PASSES` uses. A dispatch refused before its header is `c:<program>!skip<k>`.
- Consecutive draws on one queue into the same color/depth targets are one render pass: `d:<first target>#<n>`
  (`d:ps<program>#<n>` without targets). Its draws are named by their serials (`[draw-io] ... draw N`).

## Checks

Each captured pass runs alone, outside the frame, on the inputs it had in the replay:

- a dispatch through `agc_dispatch_run` (its request recompiled and dispatched directly, no driver state); a
  thread-count indirect dispatch through `agc_pass_run`;
- a render pass through `agc_pass_run`: a fresh process maps the pass's ranges at their addresses, registers its
  programs, restores the queue state of each packet (`ReplayRestoreQueueState`) and submits the packets.

Outputs are compared with what the replay left: byte-exact is `match`; images that differ are compared per element
with `agc_surface_png --compare`, and every element within `--ulps` steps of its format's precision is `near`
(reported apart from exact; buffers count as near only when every differing word is a finite float32 within the
tolerance). A mismatch runs alone a second time: two runs alone that differ make the pass `unstable` (its output
depends on GPU scheduling, as atomic append order does), not a replay difference. `error` covers runner problems
(the run alone refused the pass, read memory the capture did not save, or dropped its results).

## Scores

- **coverage** = passes with no coverage issue / passes. The issues are:
  - a refused dispatch or draw (`[gpu] skipped`, `[draw-io] ... skipped:`);
  - a program skipped by `APS5_SKIP_PROGRAMS` (`known-skip`, listed apart and not counted as the first failure);
  - descriptors bound as all zeros (`[gpu] program 0x..: N descriptor(s) bound as all zeros`, read by the driver from
    the resource plan), counted on every pass of that program;
  - a write-back that failed (`[gpu] deferred write-back failed: [<pass tag>]`);
  - guest memory changed while GPU results were pending (`keeping the CPU's blocks`);
  - a dispatch header with no bindings line (it never reached the device).
- **checked** = passes run alone / passes (per kind): how much of the frame the consistency number speaks for.
- **consistency** = checked passes that matched (exact or near) / checked passes; unstable ones are counted apart.
- **verified** = passes you marked ok / passes you judged.
- **score** = the mean over all passes of s: 0 for a pass with a coverage issue, a mismatch or a "wrong" verdict;
  otherwise 1 for "ok", 0.75 when the check matched, 0.5 when nothing is known. Printed with proven = ok / N and
  possible = (N - bad) / N, and with the checked fraction next to it.

## Driver hooks (all off unless set)

| variable | effect |
|---|---|
| `APS5_TRACE_DISPATCH_IO=1` | `[dispatch-io]` header per dispatch, and its bindings line (`shader 0x.. XxYxZ: ...`); dispatch completions carry the pass tag |
| `APS5_TRACE_DRAW_IO=1` | `[draw-io]` per draw once it is recorded (serial, targets, bindings); `[draw-io] ... skipped: reason` per refused draw |
| `APS5_TRACE_SKIPS=1` | every refused dispatch, every `APS5_SKIP_PROGRAMS` drop, null bindings per program |
| `APS5_CAPTURE_PASSES=<file>` | captures the listed passes (`<program> <instance> <dir> <ranges>` or `draws <first> <last> <dir>`), see `PassCapture.hpp` |
| `APS5_CAPTURE_PASSES_BUDGET_MIB` | no new pass starts once this much new data is written |
| `APS5_TRACE_SHADER_READS=<program hex>` | `[shader-read]` per word a resource capture of that program reads (`initial`, `lazy`, `block`, `cached`, `read`, or the pending-write policy of a word-wise page), its user data, and every address the walk found inaccessible; diff a replay against `agc_pass_run` to find a word the frame changed |

A captured pass saves every range the driver reads for it: its bindings, its targets, the memory captured at the
packet (saved from the packet-time copy; the game rewrites descriptor rings before a pipelined draw is recorded) and
everything `GuestMemory::CheckRange` accepts while the driver works on the pass (vertex inputs, decode reads).
Ranges above 64 MiB are kept as 1 MiB chunks, so passes binding the same large heap share the unchanged chunks
(`PassFiles.hpp`). A captured dispatch takes the drained path, not a pipelined commit.

Bindings print `kind 0xADDR+0xBYTES(WxH[xN[v]] fFORMAT tTILE)`. `storage(ro)` and `buffer(ro)` are read-only, and
`v` marks a volume. Draw targets print their VkFormat and ColorTileMode; textures print their guest format and
TextureTileMode. None of the hooks touches the shader recompiler, so the shader cache key stays the same.

## Tools (CMake targets, EXCLUDE_FROM_ALL)

- `agc_pass_run <pass dir>`: runs a captured pass alone through the driver (above).
- `agc_dispatch_run <shader.req> --groups X,Y,Z --inputs DIR --dump A:B`: one dispatch from its request.
- `agc_surface_png <bin> <png> --size WxH[xN[v]] (--format F --tile T | --vk-format F --color-tile C) [--slice Z]
  [--compare OTHER --ulps N]`: decodes a saved surface with the driver's layout code (`DescribeSurface`, the
  swizzle equations, `ColorTargetLayout`); with `--compare` it draws this | other | difference.
- `tools/fidelity/termimage.py`: the terminal images (sixel in Windows Terminal 1.22+, else half blocks with 24-bit
  colour; `APS5_SIXEL=0/1` overrides the guess).

## Turning a finding into a test

Every driver bug the harness finds gets a small execution test, like `tests/execution/ImageStoreVolume.cpp`:
reproduce the pass's binding shape with a hand-encoded kernel (`ps5run/tools/rdna2_encode_example.py`), run it through
the same driver path, and check guest memory.
