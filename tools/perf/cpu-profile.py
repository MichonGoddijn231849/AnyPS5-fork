#!/usr/bin/env python3
"""Sums the replay's driver CPU samples (APS5_CPU_SAMPLE=1, tools/FrameReplay.cpp) by function.

    tools/perf/cpu-profile.py [--role worker|committer] [--skip-loops N] [--top N] [--callers FUNCTION] cpu-samples.txt

Each sample is one SIGPROF (1 ms of process CPU) that landed on queue 0's worker (role 0) or the committer (role 1).
Frames are symbolized with addr2line (-f -C -i: inlined functions count as frames of their own) and the report lists,
per role, the functions by self samples (the innermost frame) and by inclusive samples (anywhere on the stack, once
per sample). With --callers (or --callees), it lists the callers of (or the calls inside) the named function (a
substring) instead.
"""
import argparse
import collections
import subprocess
import sys

ROLES = {"worker": 0, "committer": 1}


ANONYMOUS = "(anonymous namespace)"


def short_name(name):
    """The function's qualified name without its parameter list. "(anonymous namespace)" is part of the name, not a
    parameter list, so it is kept (splitting at the first "(" cut such names to their outer namespace)."""
    return name.replace(ANONYMOUS, "\0").split("(")[0].replace("\0", ANONYMOUS)


def symbolize(addresses):
    """{(module, offset): [innermost function, ..., outermost inlining function]} through one addr2line per module."""
    by_module = collections.defaultdict(set)
    for module, offset in addresses:
        by_module[module].add(offset)
    names = {}
    for module, offsets in by_module.items():
        offsets = sorted(offsets)
        if module == "?":
            for offset in offsets:
                names[(module, offset)] = ["?+0x%x" % offset]
            continue
        # -a prints each queried address before its (function, file:line) pairs: it starts a new chain.
        query = ["0x%x" % offset for offset in offsets]
        try:
            out = subprocess.run(["addr2line", "-a", "-f", "-C", "-i", "-e", module], input="\n".join(query) + "\n",
                                 capture_output=True, text=True, check=False).stdout.splitlines()
        except OSError:
            out = []
        chains = []
        i = 0
        while i < len(out):
            if out[i].startswith("0x"):
                chains.append([])
                i += 1
                continue
            if chains and out[i] != "??":
                chains[-1].append(out[i])
            i += 2
        short = module.rsplit("/", 1)[-1]
        for offset, chain in zip(offsets, chains + [[]] * (len(offsets) - len(chains))):
            names[(module, offset)] = chain if chain else ["%s+0x%x" % (short, offset)]
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--role", choices=sorted(ROLES), action="append")
    parser.add_argument("--skip-loops", type=int, default=0, help="ignore the samples of the first N written loops")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--callers", help="list the callers of the functions containing this text")
    parser.add_argument("--callees", help="list what the functions containing this text spend their samples in")
    parser.add_argument("--stacks", help="list the most common whole stacks through the functions containing this text")
    args = parser.parse_args()
    roles = [ROLES[r] for r in args.role] if args.role else [0, 1]

    samples = []
    loops = 0
    with open(args.file) as file:
        for line in file:
            if line.startswith("L "):
                loops += 1
                continue
            if not line.startswith("S ") or loops < args.skip_loops:
                continue
            fields = line.split()
            role = int(fields[1])
            if role not in roles:
                continue
            frames = []
            for frame in fields[2:]:
                module, _, offset = frame.rpartition("+0x")
                frames.append((module, int(offset, 16)))
            samples.append((role, frames))
    names = symbolize({frame for _, frames in samples for frame in frames})

    for role in roles:
        mine = [frames for r, frames in samples if r == role]
        if not mine:
            continue
        label = [k for k, v in ROLES.items() if v == role][0]
        total = len(mine)
        print("== %s: %d samples (~%d ms of CPU)" % (label, total, total))
        if args.stacks:
            stacks = collections.Counter()
            for frames in mine:
                chain = [short_name(name) for frame in frames for name in names[frame]]
                if any(args.stacks in name for name in chain):
                    stacks[" <- ".join(chain[:14])] += 1
            for stack, count in stacks.most_common(args.top):
                print("%6d %5.1f%%  %s" % (count, 100.0 * count / total, stack))
            continue
        if args.callers or args.callees:
            callers = collections.Counter()
            for frames in mine:
                chain = [name for frame in frames for name in names[frame]]
                # Outermost match first for callees (its whole subtree), innermost for callers.
                order = range(len(chain)) if args.callers else range(len(chain) - 1, -1, -1)
                for i in order:
                    if args.callers and args.callers in chain[i]:
                        callers[chain[i + 1] if i + 1 < len(chain) else "(top)"] += 1
                        break
                    if args.callees and args.callees in chain[i]:
                        callers[chain[i - 1] if i > 0 else "(self)"] += 1
                        break
            for name, count in callers.most_common(args.top):
                print("%6d %5.1f%%  %s" % (count, 100.0 * count / total, name[:160]))
            continue
        self_counts = collections.Counter()
        inclusive = collections.Counter()
        for frames in mine:
            chain = [name for frame in frames for name in names[frame]]
            if chain:
                self_counts[chain[0]] += 1
            for name in set(chain):
                inclusive[name] += 1
        print("-- self")
        for name, count in self_counts.most_common(args.top):
            print("%6d %5.1f%%  %s" % (count, 100.0 * count / total, name[:160]))
        print("-- inclusive")
        for name, count in inclusive.most_common(args.top):
            print("%6d %5.1f%%  %s" % (count, 100.0 * count / total, name[:160]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
