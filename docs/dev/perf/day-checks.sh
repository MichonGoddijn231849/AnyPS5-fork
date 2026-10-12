#!/usr/bin/env bash
# The night's checks for the day session, run on the RTX 5080 at the start of the day while the PR round happens.
#
#   docs/dev/perf/day-checks.sh [--only NAME]
#
# The night shift appends one `check` line per change below (see docs/dev/NIGHT_SHIFT.md, "Day checks"). Every
# check is a replay of the GTA V Performance capture with its switches, compared against this morning's base run
# (same build, defaults). The whole list must stay under ~30 minutes: a warm replay of 3 loops takes ~3 minutes
# here, so at most ~8 checks; combine switches that cannot interact.
#
# Results: one block per check in $OUT/summary.txt (the compare line, frame ms and CPU per draw of the last loop,
# and the log lines the check names), the logs and PNGs next to it.
#
# Environment (defaults are the day machine's):
#   BUILD    build directory with core/libs/libs/unpatched/agc_frame_replay(.exe)
#   CAPTURE  the gta-perf-3 capture (game-derived, never in a public repository)
#   MINGW    the MinGW runtime DLLs (Windows build)
#   CACHE    a warm shader cache directory for this branch
#   OUT      where results go
set -uo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${BUILD:-$root/build}"
CAPTURE="${CAPTURE:-/c/Users/Michon/ps5run/captures/gta-perf-3}"
MINGW="${MINGW:-/c/Users/Michon/tools/mingw64/bin}"
CACHE="${CACHE:-$root/build/day-check-shader-cache}"
OUT="${OUT:-$root/build/day-checks/$(date +%Y-%m-%d_%H%M)}"
only=""
[ "${1:-}" = "--only" ] && only="${2:-}"

tool="$BUILD/core/libs/libs/unpatched/agc_frame_replay"
[ -x "$tool.exe" ] && tool="$tool.exe"
mkdir -p "$OUT"
summary="$OUT/summary.txt"

# replay <name> <compare dir> [ENV=VALUE ...]
replay() {
    local name=$1 compare=$2
    shift 2
    if command -v tasklist >/dev/null && tasklist 2>/dev/null | grep -qi eboot; then
        echo "$name: the game is running; replays must not run beside it" | tee -a "$summary"
        return 1
    fi
    mkdir -p "$OUT/$name"
    local start=$SECONDS
    env "PATH=$MINGW:$BUILD/core/libs/libs/unpatched:$PATH" APS5_PROFILE_DRAW=1 "$@" \
        timeout 1800 "$tool" "$CAPTURE" --loop 3 --png "$OUT/$name" --compare "$compare" --shader-cache "$CACHE" --hidden \
        > "$OUT/$name.log" 2>&1
    local status=$?
    {
        echo "== $name (exit $status, $((SECONDS - start)) s) $*"
        grep -E "^\[compare\] (frame|[0-9]+ frames)" "$OUT/$name.log"
        grep -E "^\[replay\] loop 2: (frame ms|draws )" "$OUT/$name.log"
        grep -cE "\[gpu\] skipped|pipelined commit failed" "$OUT/$name.log" | sed 's/^/skipped or failed draws: /'
    } >> "$summary"
    return $status
}

# check <name> <what passes> <log pattern> [ENV=VALUE ...]
check() {
    local name=$1 pass=$2 pattern=$3
    shift 3
    [ -n "$only" ] && [ "$only" != "$name" ] && return 0
    replay "$name" "$OUT/base" "$@"
    {
        echo "   passes when: $pass"
        [ -n "$pattern" ] && grep -E "$pattern" "$OUT/$name.log" | tail -3 | cut -c1-300 | sed 's/^/   /'
    } >> "$summary"
}

echo "day checks of $(git -C "$root" log --oneline -1) into $OUT" | tee "$summary"
replay base "$CAPTURE/live"

# ---- The night's checks (appended by the night shift, newest last) -------------------------------------------
# Example:
# check srt-memo "frames identical to base; worker capture phase lower" "\[draw\] driver phases" APS5_SRT_EVAL_MEMO=1
check clears-and-inputs "frames identical to base; the clears line shows 0 waited (unwaited counts the clears made with no recorder); the [draws] input copies show how many vertex/index copies took ring slices instead of own buffers (0 copies: draws bind inputs in place and the ring changes nothing)" "clears [0-9]+ recorded|input copies|frame ms:" APS5_UNWAITED_CLEARS=1 APS5_DRAW_INPUT_RING=1

echo "done: $summary"
