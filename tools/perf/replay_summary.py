#!/usr/bin/env python3
"""One line from an agc_frame_replay log run with APS5_PROFILE_DRAW=1 (tools/perf/replay-bench.sh).

The per-loop lines give the frame CPU and the per-draw CPU of queue 0's worker and of the committer; the last
10-second profile windows give their phases. Loops before --skip (warm-up: shader builds, first templates) are left
out of the averages.
"""
import argparse
import re
import statistics
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--skip", type=int, default=3, help="warm-up loops left out")
    parser.add_argument("--label", default="")
    args = parser.parse_args()
    text = open(args.log, errors="replace").read()

    draws = {}
    for loop, packets, committed, per_frame, worker_us, committer_us in re.findall(
            r"\[replay\] loop (\d+): draws (\d+) packets, (\d+) committed \(([\d.]+) per frame\); worker ([\d.]+) us per packet, committer ([\d.]+) us per committed draw", text):
        draws[int(loop)] = (int(packets), int(committed), float(per_frame), float(worker_us), float(committer_us))
    frame_cpu = {}
    for loop, role, total, frames in re.findall(r"\[replay\] loop (\d+): (queue 0 worker|committer) cpu ms per frame:.*?\(([\d.]+) ms in (\d+) frames\)", text):
        frame_cpu.setdefault(int(loop), {})[role] = float(total) / max(int(frames), 1)
    batches = {int(loop): float(per_frame) for loop, per_frame in re.findall(r"\[replay\] loop (\d+): \d+ recorded batches submitted \(([\d.]+) per frame\)", text)}
    loops = sorted(loop for loop in draws if loop >= args.skip)
    if not loops:
        print("no measured loops (fewer than --skip + 1 in the log?)", file=sys.stderr)
        return 1

    def mean(values):
        return statistics.fmean(values) if values else 0.0

    def spread(values):
        return statistics.pstdev(values) if len(values) > 1 else 0.0

    worker_us = [draws[loop][3] for loop in loops]
    committer_us = [draws[loop][4] for loop in loops]
    worker_ms = [frame_cpu[loop].get("queue 0 worker", 0.0) for loop in loops if loop in frame_cpu]
    committer_ms = [frame_cpu[loop].get("committer", 0.0) for loop in loops if loop in frame_cpu]
    per_frame = mean([draws[loop][2] for loop in loops])
    batches_per_frame = mean([batches[loop] for loop in loops if loop in batches])

    # The committer's phases: the last full [draws] window (ms per phase over its draw count).
    committer_phases = ""
    windows = re.findall(r"\[draws\] (\d+) draws over 10 s .*?: (validate=.*?keep=[\d.]+ms)", text)
    if windows:
        count, phases = windows[-1]
        count = max(int(count), 1)
        parts = []
        for name, ms in re.findall(r"(\w+)=([\d.]+)ms", phases):
            parts.append(f"{name} {float(ms) * 1000.0 / count:.1f}")
        committer_phases = " ".join(parts)
    # The worker's phases: the last driver-phases window (us per packet).
    worker_phases = ""
    phase_windows = re.findall(r"\[draw\] driver phases \(10 s, .*?\), us per packet: (.*?), total ([\d.]+)", text)
    if phase_windows:
        phases, total = phase_windows[-1]
        keep = []
        for name, us in re.findall(r"([a-zA-Z/:\- ]+?) ([\d.]+)", phases):
            if float(us) >= 0.5:
                keep.append(f"{name.strip()} {float(us):.1f}")
        worker_phases = " ".join(keep) + f" (total {float(total):.1f})"

    label = f"{args.label}: " if args.label else ""
    print(f"{label}{len(loops)} loops, {per_frame:.0f} draws/frame, {batches_per_frame:.1f} batches/frame; frame CPU worker {mean(worker_ms):.1f} ms committer {mean(committer_ms):.1f} ms; "
          f"us/draw worker {mean(worker_us):.2f} (+-{spread(worker_us):.2f}) committer {mean(committer_us):.2f} (+-{spread(committer_us):.2f}) | "
          f"committer phases us/draw: {committer_phases} | worker phases us/packet: {worker_phases}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
