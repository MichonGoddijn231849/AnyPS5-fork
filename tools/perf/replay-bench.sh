#!/usr/bin/env bash
# Times the driver's CPU path on the GTA V Performance capture (docs/dev/NIGHT_SHIFT.md B0), on a machine whose GPU
# could not run the frame: APS5_NULL_SUBMIT=1 replays every draw without executing GPU work.
#
#   tools/perf/replay-bench.sh [--loops N] [--skip N] [--label TEXT] [--no-build] [--log FILE] [--sample [N]]
#                              [-- ENV=VALUE ...]
#
# Builds agc_frame_replay in build-perf (unless --no-build), runs N loops under Xvfb with APS5_PROFILE_DRAW=1 and
# prints one line: draws per frame, the worker's and the committer's CPU per frame and per draw (mean and spread over
# the loops after the warm-up), and their phases from the last 10-second profile window. Extra ENV=VALUE pairs after
# `--` go to the replay, so an A/B is two runs:
#
#   tools/perf/replay-bench.sh --label base
#   tools/perf/replay-bench.sh --label split -- APS5_SPLIT_COMMIT=2
#
# --sample [N] also profiles the driver threads (APS5_CPU_SAMPLE, Linux) and prints the N (default 15) functions with
# the most self samples for the committer and for the worker (tools/perf/cpu-profile.py; the samples file is kept
# next to the log for --callers / --callees / --stacks).
#
# Needs: testdata/gta-perf-3/{events,pages}.bin (git lfs pull --include="testdata/gta-perf-3/*", then unxz -k),
# lavapipe (source ~/hwrt-env.sh), Xvfb. Raises vm.max_map_count and drops the page cache when allowed (lavapipe maps
# many objects; the container's memory limit counts the page cache).
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
loops=20
skip=3
label=""
build=1
log=""
env_pairs=()
sample=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --loops) loops="$2"; shift 2 ;;
        --skip) skip="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        --no-build) build=0; shift ;;
        --log) log="$2"; shift 2 ;;
        --sample)
            sample=15
            if [[ $# -gt 1 && "$2" =~ ^[0-9]+$ ]]; then sample="$2"; shift; fi
            shift ;;
        --) shift; env_pairs=("$@"); break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

capture="$root/testdata/gta-perf-3"
for file in events.bin pages.bin; do
    if [[ ! -f "$capture/$file" ]]; then
        echo "missing $capture/$file: git lfs pull --include=\"testdata/gta-perf-3/*\" && unxz -k $capture/*.xz" >&2
        exit 1
    fi
done
[[ -f "$HOME/hwrt-env.sh" ]] && source "$HOME/hwrt-env.sh" >/dev/null 2>&1 || true

if [[ $build -eq 1 ]]; then
    cmake --build "$root/build-perf" --target agc_frame_replay -j"$(nproc)" >/dev/null
fi
replay="$(find "$root/build-perf" -name agc_frame_replay -type f -perm -u+x | head -1)"
[[ -n "$replay" ]] || { echo "agc_frame_replay is not built in build-perf" >&2; exit 1; }

if [[ "$(cat /proc/sys/vm/max_map_count)" -lt 1048576 ]]; then
    sysctl -qw vm.max_map_count=1048576 2>/dev/null || echo "warning: vm.max_map_count is low; lavapipe may run out of mappings" >&2
fi
sync
echo 1 > /proc/sys/vm/drop_caches 2>/dev/null || true

display="${DISPLAY:-}"
xvfb_pid=""
if [[ -z "$display" ]]; then
    display=":97"
    Xvfb "$display" -screen 0 1920x1080x24 >/dev/null 2>&1 &
    xvfb_pid=$!
    sleep 1
fi
cleanup() { [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true; }
trap cleanup EXIT

[[ -n "$log" ]] || log="$(mktemp --suffix=.replay.log)"
sample_env=()
samples="${log%.log}.samples.txt"
[[ $sample -gt 0 ]] && sample_env=(APS5_CPU_SAMPLE=1 "APS5_CPU_SAMPLE_FILE=$samples")
env DISPLAY="$display" APS5_NULL_SUBMIT=1 APS5_PROFILE_DRAW=1 "${sample_env[@]}" "${env_pairs[@]}" \
    "$replay" "$capture" --loop "$loops" --hidden >"$log" 2>&1 || { echo "replay failed; log: $log" >&2; tail -5 "$log" >&2; exit 1; }
python3 "$root/tools/perf/replay_summary.py" "$log" --skip "$skip" --label "$label"
if [[ $sample -gt 0 ]]; then
    for role in committer worker; do
        python3 "$root/tools/perf/cpu-profile.py" --role "$role" --top "$sample" "$samples" | sed -n '1p;/-- self/,/-- inclusive/p' | grep -v -- '-- inclusive' | cut -c1-160
    done
    echo "samples: $samples"
fi
