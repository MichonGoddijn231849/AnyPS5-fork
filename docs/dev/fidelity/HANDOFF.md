# Fidelity harness: handoff (2026-10-11)

State of the rendering-correctness harness (`tools/fidelity`, details in `docs/dev/Fidelity.md`) at the end of
the session that built it, for the session that continues. Branch `night/merge`, local, not pushed.

## Commands

From `C:\Users\Michon\AnyPS5-merge`, Python `C:\Users\Michon\tools\python\python.exe`, build in `build/`
(`ninja agc_frame_replay agc_pass_run agc_dispatch_run agc_surface_png`; the four tools are EXCLUDE_FROM_ALL).
One frame replay at a time (~17 GB): message michon-d9 (AnyPS5-prfix) before and after each replay.

```
F="tools/fidelity/fidelity.py --capture wolverine-cutscene"
python $F inventory --run                    # traced replay -> inventory.json (~3.5 min)
python $F capture --passes ID,ID --again     # capture chosen passes (one replay); without --passes: unchecked passes in frame order
python $F capture --limit 450 --budget-gib 6 # the next 450 unchecked passes in frame order
python $F check [--passes ID,...] [--again] [--ulps 4] [--prune]   # run captured passes alone and compare (no replay)
python $F sweep --rounds 6 --limit 450 --budget-gib 6              # capture -> check -> prune, one replay per round (~10 min)
python $F report [--live] [--json]
python $F view ID [--slice Z] [--inputs] [--mode auto|sixel|blocks|png]
python $F view next --ask                    # next mismatch (else captured pass) without a verdict, then asks ok/wrong
python $F verdict ID ok|wrong [--note ..]; python $F finding ID "text"
```

`agc_pass_run` alone (what `check` runs for draws and thread-indirect dispatches):

```
export PATH=/c/Users/Michon/tools/mingw64/bin:/c/Users/Michon/AnyPS5-merge/build/core/libs/libs/unpatched:$PATH
export APS5_WIDE_BINDLESS=1 APS5_SUBROUTINE_CALLS=1 APS5_BINDLESS_STORAGE_TABLES=1 APS5_NULL_UNRESOLVED_IMAGES=1
agc_pass_run.exe <work>/passes/<id> --out <dir> [--blobs <work>/capture-passes.txt.blobs]
# add APS5_TRACE_DRAW_IO=1 / APS5_TRACE_DISPATCH_IO=1 / APS5_TRACE_UNREADABLE=1 to see what it ran or failed to read
```

## Where things live

- Work dir: `C:\Users\Michon\ps5run\captures\fidelity\wolverine-cutscene\`
  - `config.json` (replay exe, args, the Wolverine env set, `known_skips`), `inventory.json`, `checks.jsonl`,
    `captured.json`, `verdicts.json`, `findings.json`;
  - `status.json`: the scores, written atomically; poll it to report progress;
  - `passes/<id>/` (before/, after/, packets/, shaders/, ranges.txt, isolated/ or offline/),
    `capture-passes.txt.blobs/` (content-addressed range data), `views/` (PNGs), `run-inventory/`, `run-capture/`.
- Capture: `C:\Users\Michon\ps5run\captures\wolverine-cutscene` (the frame replayed).
- Disk budget: `--budget-gib` (default 6) for new data per capture run, passed to the driver as
  `APS5_CAPTURE_PASSES_BUDGET_MIB`; the harness also keeps 5 GiB of the disk free. `check --prune` / `sweep` delete
  the data of matched passes and unreferenced blobs. C: had ~12 GB free.

## State (Wolverine cutscene)

3295 passes; coverage 98.88%; checked 763 (738 of 3228 dispatches, 25 of 67 render passes); 759 exact, 0 near,
22 unstable (nondeterministic alone), 4 mismatches; about 2500 passes unchecked (`sweep` continues in frame order).

## Findings (not investigated)

Both passes leave their output unchanged in the replay while the pass run alone writes it, and they are the only two
passes with `[gpu] deferred write-back failed: BDA access failed: address=0x0 instruction=0x0 bytes=4 stage=4
reason=1` attributed to them (Recorder completion tag):

- `c:11d02d3b00#0` (fog injection, frame index 933): volumes 0x117ef40000 and 0x117f5c0000 (160x90x64, B10G11R11);
  replay changed 0 bytes, alone 2751096 and 3682587. `view c:11d02d3b00#0`.
- `c:11d023c300#0` (indirect dispatch, queue 0, submission 68): target 0x113d770000 (2560x1440); replay changed 0,
  alone 284038 elements. `view c:11d023c300#0`.

The BDA fault check runs in `ShaderResources::WriteBackBuffers` (guards buffer write-back); the outputs are storage
images, so the fault is a lead, not a proven cause. The other two mismatches (`c:7ff6af15ea00#0`,
`c:7ff6af165600#0`, eboot programs) were not looked at.

## Open runner gaps (reported as `error`, never as mismatch)

- `c:7ff6af226c00` (162 passes): calls a subroutine through a computed target (`s_swappc_b64` at pc 0xcc, callee
  0x11d039d800); alone the driver throws "raw compute artifact does not match its invocation"
  (`ShaderRegistry.cpp`, prepared-at-use key mismatch). The capture already saves the callee's snapshot.
- 12 render passes (`d:1129ec0000#*`, `d:113ac60000#0`, `d:114b5c0000#0`): alone the driver logs "guest memory
  changed while GPU results were pending; keeping the CPU's blocks (cpu wrote 1)": the restored target memory counts as
  CPU writes even after `ReplayCollectWrites`, and the GPU results are dropped. `d:1129ec0000#0` (20 packets) matched,
  so it depends on the pass.
- UAV-only draws (`d:ps11d01b9100#*`, `d:ps11d01b8d00#*`, no render targets, pixel shader writes a storage image):
  alone they write nothing ("the run alone wrote nothing where the replay wrote").

## Machine state

- Two `agc_pass_run.exe` processes are stuck in process teardown (PIDs 14472 and 6624, from before the runner exited
  with TerminateProcess); they cannot be killed and hold renamed files in `build\core\libs\libs\unpatched`:
  `libSceAgcDriver.prx.locked-14472`, `libSceAgcDriver.prx.locked-6624`, `agc_pass_run.exe.locked-14472`,
  `agc_pass_run.exe.locked-6624`. Delete them after a reboot.
- Shader cache folders under `C:\Users\Michon\ps5run\wolverine\shader_cache` (not deleted, the user decides):
  `768cc4d33f140c0f` (3.6 GB, created by a recompiler change since reverted; unused now) and `594e36aaff315eca`
  (1.8 GB, 14:25, predates this harness's replays). The current key is `624f8b84c2120d67`.
