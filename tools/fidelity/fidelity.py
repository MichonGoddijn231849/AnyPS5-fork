#!/usr/bin/env python3
"""Rendering-correctness harness for frame captures (any game).

Subcommands (see docs/dev/Fidelity.md):
  inventory  --log FILE          parse a traced replay log into inventory.json (one entry per pass, in frame order)
  inventory  --run               run the capture's replay with the inventory traces and parse its log while it runs
  report     [--live]            print the scores (and keep redrawing them with --live); always rewrites status.json
  verdict    PASS ok|wrong|unknown [--note TEXT]

Every subcommand takes --dir DIR: the per-capture work directory (default ps5run/captures/fidelity/<capture>),
which holds inventory.json, checks.jsonl, verdicts.json, progress.json and status.json.

The pass list comes from driver trace lines only:
  [dispatch-io] queue 0xQ submission S groups XxYxZ shader 0xP        (Dispatch.cpp, APS5_TRACE_DISPATCH_IO)
  [dispatch-io] XxYxZ|indirect: <resources>                           (VulkanDevice.cpp, the dispatch's bindings)
  [draw-io] ...                                                       (Draw.cpp / PacketExecution.cpp, APS5_TRACE_DRAW_IO)
and the driver's own refusal and fallback messages (ISSUE_PATTERNS below).
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

PS5RUN = Path(os.environ.get("APS5_PS5RUN", r"C:\Users\Michon\ps5run"))
# The embedded tools Python does not put the script's directory on the path (termimage.py lives next to this file).
sys.path.insert(0, str(Path(__file__).resolve().parent))

# --------------------------------------------------------------------------------------------------------------------
# Score
# --------------------------------------------------------------------------------------------------------------------

# Per pass: 0 when it is known bad (a coverage issue, an offline/replay mismatch or a "wrong" verdict), 1 for an "ok"
# verdict, 0.75 when the offline run matched the replay, 0.5 when nothing is known yet. The score is the mean.
SCORE_BAD, SCORE_UNKNOWN, SCORE_CONSISTENT, SCORE_OK = 0.0, 0.5, 0.75, 1.0
SCORE_FORMULA = ("score = mean over passes of s; s = 0 if the pass has a skip/fallback/null-binding issue, an "
                 "offline-vs-replay mismatch or a 'wrong' verdict; else 1 for an 'ok' verdict, 0.75 if the offline "
                 "check matched, 0.5 if nothing is known. proven = ok/N, possible = (N - bad)/N.")

# --------------------------------------------------------------------------------------------------------------------
# Driver messages that mean a pass did not run as the game asked: (type, regex, whether it costs coverage).
# Groups: 'program' ties the message to that program's passes; 'tag' is a pass tag (Graphics::PassTag: "program 0x.."
# for a dispatch, a draw's [draw-io] head); 'address' ties it to the latest pass writing that address. Anything else
# stays unattributed and is listed on its own.
# --------------------------------------------------------------------------------------------------------------------
ISSUE_PATTERNS = [
    ("skip", re.compile(r"^\[gpu\] skipped dispatch(?: again)?: compute shader 0x(?P<program>[0-9a-f]+)(?: \([^)]*\))?: (?P<reason>.*)$"), True),
    ("skip", re.compile(r"^\[gpu\] skipped draw(?: again)?: (?P<reason>.*?)(?: \[(?P<packet>\w+), color target 0x(?P<target>[0-9a-f]+)\])?$"), True),
    ("null-binding", re.compile(r"^\[gpu\] program 0x(?P<program>[0-9a-f]+): (?P<reason>\d+ descriptor\(s\) bound as all zeros.*)$"), True),
    ("writeback-failed", re.compile(r"^\[gpu\] deferred write-back failed: (?:\[(?P<tag>[^\]]*)\] )?(?P<reason>.*)$"), True),
    ("cpu-blocks-kept", re.compile(r"^\[gpu\] storage image 0x(?P<address>[0-9a-f]+): (?P<reason>guest memory changed while GPU results were pending.*)$"), True),
]

# --------------------------------------------------------------------------------------------------------------------
# Inventory
# --------------------------------------------------------------------------------------------------------------------

HEADER = re.compile(r"^\[dispatch-io\] queue 0x(?P<queue>[0-9a-f]+) submission (?P<sub>\d+) groups (?P<groups>\d+x\d+x\d+) shader 0x(?P<program>[0-9a-f]+)")
RESOURCES = re.compile(r"^\[dispatch-io\] (?:shader 0x(?P<program>[0-9a-f]+) )?(?P<groups>indirect|\d+x\d+x\d+):(?P<rest>.*)$")
DRAW = re.compile(r"^\[draw-io\] queue 0x(?P<queue>[0-9a-f]+) submission (?P<sub>\d+) (?:draw (?P<serial>\d+) )?(?P<packet>\S+) ps 0x(?P<ps>[0-9a-f]+) vs 0x(?P<vs>[0-9a-f]+)(?P<rest>.*)$")
LOOP_END = re.compile(r"^\[replay\] loop (?P<loop>\d+): \d+ submissions")
DRAW_PACKETS = re.compile(r"^\[replay\] loop 0: draws (?P<packets>\d+) packets")
IMAGE = re.compile(r"(?P<kind>texture|storage\(ro\)|storage|color|depth) 0x(?P<addr>[0-9a-f]+)\+0x(?P<bytes>[0-9a-f]+)\((?P<w>\d+)x(?P<h>\d+)(?:x(?P<d>\d+)(?P<volume>v?))? f(?P<fmt>\d+) t(?P<tile>-?\d+)\)(?: nz=(?P<nz>-?[0-9.]+))?")
BUFFER = re.compile(r"(?P<kind>buffer(?:\(ro\))?) 0x(?P<addr>[0-9a-f]+)\+0x(?P<bytes>[0-9a-f]+)(?: nz=(?P<nz>-?[0-9.]+))?")
TAG_PROGRAM = re.compile(r"(?:^program|\bps) 0x(?P<program>[0-9a-f]+)")


def parse_resources(text: str) -> tuple[list[dict], list[dict]]:
    """Inputs and outputs of one pass from its Describe() text. Storage images and written buffers count as outputs
    (they may be read too); a null texture (address 0) is an input like any other."""
    inputs, outputs = [], []
    for match in IMAGE.finditer(text):
        kind = match["kind"]
        entry = {"kind": kind.replace("(ro)", ""), "addr": int(match["addr"], 16), "bytes": int(match["bytes"], 16), "w": int(match["w"]),
                 "h": int(match["h"]), "d": int(match["d"] or 1), "volume": bool(match["volume"]), "fmt": int(match["fmt"]), "tile": int(match["tile"])}
        if match["nz"] is not None: entry["nz"] = float(match["nz"])
        if kind in ("texture", "storage(ro)"): inputs.append(entry)
        else: outputs.append(entry)
    for match in BUFFER.finditer(text):
        entry = {"kind": "buffer", "addr": int(match["addr"], 16), "bytes": int(match["bytes"], 16)}
        if match["nz"] is not None: entry["nz"] = float(match["nz"])
        if match["kind"] == "buffer(ro)": inputs.append(entry)
        else: outputs.append(entry)
    return inputs, outputs


@dataclass
class Command:
    index: int
    kind: str                       # dispatch | draw
    program: int                    # compute program, or the pixel program of a draw
    queue: int = -1
    submission: int = -1
    groups: str = ""
    packet: str = ""
    vs: int = 0
    targets: tuple = ()             # draw: color targets and depth (addresses), for grouping
    inputs: list = field(default_factory=list)
    outputs: list = field(default_factory=list)
    issues: list = field(default_factory=list)
    line: int = 0
    paired: bool = False
    serial: int = -1                # a draw's serial ([draw-io] "draw N", APS5_CAPTURE_PASSES)

    def programs(self) -> tuple:
        return (self.program, self.vs) if self.kind == "draw" else (self.program,)

    def add(self, issue: dict) -> None:
        if not any(i["type"] == issue["type"] and i["text"] == issue["text"] for i in self.issues): self.issues.append(issue)


class Inventory:
    """Incremental parser: feed() lines as the replay prints them, then passes() at any time."""

    def __init__(self, known_skips: dict | None = None) -> None:
        self.commands: list[Command] = []
        self.pending: list[Command] = []        # dispatch headers waiting for their resource line
        self.unattributed: list[dict] = []
        self.program_issues: dict[int, list[dict]] = {}    # issues of a program (null bindings): every pass of it
        self.known_skips = {int(k, 16): v for k, v in (known_skips or {}).items()}
        self.lines = 0
        self.loops_done = 0
        self.stopped = False                    # only loop 0 is the frame; later loops repeat it
        self.draw_packets = None                # the replay's own count, to tell whether draws were traced
        self.draw_io = False                    # [draw-io] lines seen: [gpu] skipped draw lines are then duplicates

    def feed(self, line: str) -> None:
        if (match := DRAW_PACKETS.match(line)): self.draw_packets = int(match["packets"])
        if self.stopped: return
        self.lines += 1
        line = line.rstrip("\r\n")
        if not line.startswith("["): return
        if line.startswith("[dispatch-io]"):
            if (match := HEADER.match(line)):
                self.append(Command(len(self.commands), "dispatch", int(match["program"], 16), int(match["queue"], 16), int(match["sub"]), match["groups"], line=self.lines))
                self.pending.append(self.commands[-1])
            elif (match := RESOURCES.match(line)):
                self.pair(match)
            return
        if line.startswith("[draw-io]"):
            if (match := DRAW.match(line)):
                self.draw_io = True
                rest, _, skipped = match["rest"].partition(" skipped: ")
                bindings = rest if not skipped else rest.split(":", 1)[0]
                inputs, outputs = parse_resources(bindings)
                targets = tuple(sorted(o["addr"] for o in outputs if o["kind"] in ("color", "depth")))
                command = Command(len(self.commands), "draw", int(match["ps"], 16), int(match["queue"], 16), int(match["sub"]), packet=match["packet"],
                                  vs=int(match["vs"], 16), targets=targets, inputs=inputs, outputs=outputs, line=self.lines, paired=True,
                                  serial=int(match["serial"]) if match["serial"] else -1)
                if skipped: command.add({"type": "skip", "text": skipped[:240], "coverage": True})
                self.append(command)
            return
        if (match := LOOP_END.match(line)):
            self.loops_done = int(match["loop"]) + 1
            self.stopped = True
            return
        for kind, pattern, coverage in ISSUE_PATTERNS:
            if (match := pattern.match(line)):
                self.issue(kind, match, coverage, line)
                return

    def append(self, command: Command) -> None:
        for program in command.programs():
            for issue in self.program_issues.get(program, []): command.add(issue)
        self.commands.append(command)

    def pair(self, match: re.Match) -> None:
        groups = "0x0x0" if match["groups"] == "indirect" else match["groups"]
        program = int(match["program"], 16) if match["program"] else None
        for i, command in enumerate(self.pending):
            if (program is None or command.program == program) and command.groups == groups:
                command.inputs, command.outputs = parse_resources(match["rest"])
                command.paired = True
                del self.pending[i]
                return
        if len(self.pending) > 64: self.pending = self.pending[-64:]

    def issue(self, kind: str, match: re.Match, coverage: bool, line: str) -> None:
        groups = match.groupdict()
        issue = {"type": kind, "text": (groups.get("reason") or line)[:240], "coverage": coverage}
        if kind == "skip" and groups.get("program"):
            # A refused dispatch prints no [dispatch-io] header: it is a pass of its own at this point.
            program = int(groups["program"], 16)
            if program in self.known_skips or issue["text"] == "APS5_SKIP_PROGRAMS":
                issue["type"] = "known-skip"
                issue["text"] = self.known_skips.get(program, issue["text"])
            command = Command(len(self.commands), "dispatch", program, line=self.lines, paired=True)
            command.add(issue)
            self.append(command)
            return
        if kind == "skip":
            if self.draw_io: return             # the [draw-io] skipped line already counted it
            target = int(groups["target"], 16) if groups.get("target") else 0
            command = Command(len(self.commands), "draw", 0, packet=groups.get("packet") or "", targets=(target,) if target else (), line=self.lines, paired=True)
            if target: command.outputs = [{"kind": "color", "addr": target, "bytes": 0}]
            command.add(issue)
            self.append(command)
            return
        if groups.get("program"):
            # A property of the program (bound when it was prepared, often before the frame): all of its passes.
            program = int(groups["program"], 16)
            self.program_issues.setdefault(program, []).append(issue)
            for command in self.commands:
                if program in command.programs(): command.add(issue)
            return
        if groups.get("tag") and (tagged := TAG_PROGRAM.search(groups["tag"])):
            program = int(tagged["program"], 16)
            for command in reversed(self.commands):
                if command.program == program:
                    command.add(issue)
                    return
            issue["program"] = program
        elif groups.get("address"):
            address = int(groups["address"], 16)
            for command in reversed(self.commands):
                if any(r["addr"] <= address < r["addr"] + max(r["bytes"], 1) for r in command.outputs):
                    command.add(issue)
                    return
            issue["address"] = address
        self.unattributed.append(issue)

    def finish(self) -> None:
        """At the end of the frame: a dispatch whose header never got its bindings line did not reach the device."""
        for command in self.commands:
            if command.kind == "dispatch" and not command.paired:
                command.add({"type": "no-bindings", "text": "header without a [dispatch-io] bindings line: the dispatch did not reach the device", "coverage": True})

    def passes(self) -> list[dict]:
        """Dispatches are one pass each; consecutive draws on one queue into the same targets are one render pass."""
        passes: list[dict] = []
        seen: dict[str, int] = {}
        current = None
        for command in self.commands:
            if command.kind == "draw" and current is not None and current["kind"] == "draw" and current["queue"] == command.queue and command.targets and tuple(current["targets"]) == command.targets:
                current["draws"] += 1
                current["programs"] = sorted(set(current["programs"]) | {command.program})
                current["inputs"] = merge_ranges(current["inputs"] + command.inputs)
                current["outputs"] = merge_ranges(current["outputs"] + command.outputs)
                current["issues"] += [i for i in command.issues if i not in current["issues"]]
                current["serials"].append(command.serial)
                if not any(i["type"] == "skip" for i in command.issues): current["drawn"].append(command.serial)
                continue
            # A dispatch's number counts the program's [dispatch-io] headers, as the driver's APS5_CAPTURE_PASSES instance
            # does; a dispatch refused before its header is numbered apart (c:<program>!skip<k>).
            if command.kind == "dispatch":
                base = f"c:{command.program:x}" + ("#" if command.queue >= 0 else "!skip")
            else:
                base = (f"d:{command.targets[0]:x}" if command.targets else f"d:ps{command.program:x}") + "#"
            n = seen.get(base, 0)
            seen[base] = n + 1
            current = {"id": f"{base}{n}", "instance": n, "index": len(passes), "kind": command.kind, "queue": command.queue, "submission": command.submission,
                       "program": command.program, "programs": [command.program], "groups": command.groups, "packet": command.packet,
                       "targets": list(command.targets), "draws": 1 if command.kind == "draw" else 0, "inputs": list(command.inputs),
                       "outputs": list(command.outputs), "issues": list(command.issues), "line": command.line, "traced": command.paired}
            if command.kind == "draw":
                current["serials"] = [command.serial]
                current["drawn"] = [] if any(i["type"] == "skip" for i in command.issues) else [command.serial]
            passes.append(current)
        return passes


def merge_ranges(ranges: list[dict]) -> list[dict]:
    unique = {}
    for r in ranges: unique.setdefault((r["kind"], r["addr"], r["bytes"]), r)
    return list(unique.values())


def write_json(path: Path, data) -> None:
    """Atomic: a reader polling the file never sees half of it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=1, default=hex_default), encoding="utf-8")
    for attempt in range(20):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:     # a reader has it open on Windows
            time.sleep(0.05)
    os.replace(temporary, path)


def hex_default(value):
    raise TypeError(f"not JSON: {value!r}")


def save_inventory(work: Path, inventory: Inventory, source: str, finished: bool) -> dict:
    passes = inventory.passes()
    data = {"source": source, "finished": finished, "loops_done": inventory.loops_done, "lines": inventory.lines,
            "dispatches": sum(c.kind == "dispatch" for c in inventory.commands), "draws": sum(c.kind == "draw" for c in inventory.commands),
            "draw_packets": inventory.draw_packets, "untraced": sum(not p["traced"] for p in passes), "passes": passes, "unattributed": inventory.unattributed}
    write_json(work / "inventory.json", data)
    return data


# --------------------------------------------------------------------------------------------------------------------
# Scoring and the status file
# --------------------------------------------------------------------------------------------------------------------

def load_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, PermissionError):
        return default


def load_checks(work: Path) -> dict[str, dict]:
    checks = {}
    try:
        with open(work / "checks.jsonl", encoding="utf-8") as file:
            for line in file:
                try:
                    entry = json.loads(line)
                    checks[entry["pass"]] = entry
                except (json.JSONDecodeError, KeyError):
                    pass
    except FileNotFoundError:
        pass
    return checks


def overlaps(a: dict, b: dict) -> bool:
    return a["addr"] < b["addr"] + max(b["bytes"], 1) and b["addr"] < a["addr"] + max(a["bytes"], 1)


def evaluate(work: Path) -> dict:
    inventory = load_json(work / "inventory.json", {"passes": [], "unattributed": []})
    checks = load_checks(work)
    verdicts = load_json(work / "verdicts.json", {})
    progress = load_json(work / "progress.json", {})
    passes = inventory["passes"]
    rows = []
    totals = {"covered": 0, "checked": 0, "consistent": 0, "exact": 0, "near": 0, "unstable": 0, "judged": 0, "ok": 0, "bad": 0, "known": 0}
    by_kind = {"dispatch": [0, 0], "draw": [0, 0]}     # checked, passes
    score_sum = 0.0
    bad_outputs: list[dict] = []
    affected = 0
    first = None
    by_type: dict[str, int] = {}
    for p in passes:
        reasons = []
        # Known skips (the run's own APS5_SKIP_PROGRAMS) cost coverage but are listed after the unknown issues.
        coverage_issues = sorted((i for i in p["issues"] if i.get("coverage", True)), key=lambda i: i["type"] == "known-skip")
        for kind in {i["type"] for i in coverage_issues}: by_type[kind] = by_type.get(kind, 0) + 1
        covered = not coverage_issues
        if covered: totals["covered"] += 1
        else: reasons += [f"{i['type']}: {i['text']}" for i in coverage_issues]
        known = bool(coverage_issues) and all(i["type"] == "known-skip" for i in coverage_issues)
        check = checks.get(p["id"])
        by_kind.setdefault(p["kind"], [0, 0])[1] += 1
        if check is not None and check.get("result") == "unstable": totals["unstable"] += 1
        if check is not None and check.get("result") in ("match", "near", "mismatch"):
            totals["checked"] += 1
            by_kind[p["kind"]][0] += 1
            if check["result"] != "mismatch":
                totals["consistent"] += 1
                totals["exact" if check["result"] == "match" else "near"] += 1
            else: reasons.append(f"offline != replay: {check.get('detail', '')}")
        verdict = verdicts.get(p["id"], {}).get("verdict", "unknown")
        if verdict in ("ok", "wrong"):
            totals["judged"] += 1
            if verdict == "ok": totals["ok"] += 1
            else: reasons.append(f"verdict wrong: {verdicts[p['id']].get('note', '')}")
        if reasons: s = SCORE_BAD
        elif verdict == "ok": s = SCORE_OK
        elif check is not None and check.get("result") in ("match", "near"): s = SCORE_CONSISTENT
        else: s = SCORE_UNKNOWN
        score_sum += s
        # Downstream of a bad pass: reads what a bad (or already affected) pass wrote.
        downstream = not reasons and any(overlaps(i, o) for i in p["inputs"] for o in bad_outputs)
        if reasons:
            totals["bad"] += 1
            if known and len(reasons) == len(coverage_issues): totals["known"] += 1
            elif first is None: first = {"index": p["index"], "id": p["id"], "kind": p["kind"], "reason": reasons[0]}
        if downstream: affected += 1
        if (reasons or downstream) and len(bad_outputs) < 4096: bad_outputs += p["outputs"]
        rows.append({"id": p["id"], "s": s, "reasons": reasons, "downstream": downstream, "known": known and len(reasons) == len(coverage_issues)})
    n = len(passes)
    pct = lambda a, b: round(100.0 * a / b, 2) if b else None
    status = {
        "updated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "capture": inventory.get("source", ""),
        "inventory_finished": inventory.get("finished", False),
        "phase": progress.get("phase", "idle"),
        "progress": progress,
        "passes": n,
        "dispatches": inventory.get("dispatches", 0),
        "draws": inventory.get("draws", 0),
        "draw_packets": inventory.get("draw_packets"),
        "covered": totals["covered"], "checked": totals["checked"], "consistent": totals["consistent"],
        "exact": totals["exact"], "within_tolerance": totals["near"], "unstable": totals["unstable"], "checked_pct": pct(totals["checked"], n),
        "checked_by_kind": {k: {"checked": v[0], "passes": v[1]} for k, v in by_kind.items()},
        "judged": totals["judged"], "ok": totals["ok"], "bad": totals["bad"], "known_bad": totals["known"], "downstream_of_bad": affected,
        "coverage_pct": pct(totals["covered"], n),
        "consistency_pct": pct(totals["consistent"], totals["checked"]),
        "verified_pct": pct(totals["ok"], totals["judged"]),
        "score_pct": pct(score_sum, n),
        "proven_pct": pct(totals["ok"], n),
        "possible_pct": pct(n - totals["bad"], n),
        "formula": SCORE_FORMULA,
        "issues_by_type": by_type,
        "unattributed": len(inventory.get("unattributed", [])),
        "first_failure": first,
        "failures": [{"id": r["id"], "reason": r["reasons"][0]} for r in rows if r["reasons"] and not r["known"]][:50],
        "known_failures": sorted({r["reasons"][0] for r in rows if r["known"]}),
        "findings": load_json(work / "findings.json", []),
    }
    write_json(work / "status.json", status)
    return status


# --------------------------------------------------------------------------------------------------------------------
# Terminal view
# --------------------------------------------------------------------------------------------------------------------

def bar(value, width=30) -> str:
    if value is None: return "[" + " " * width + "]   n/a"
    filled = int(round(value / 100 * width))
    return "[" + "#" * filled + "." * (width - filled) + f"] {value:6.2f}%"


def render(status: dict, inventory: dict | None = None) -> str:
    lines = []
    progress = status.get("progress") or {}
    phase = status.get("phase", "idle")
    head = f"fidelity  {status.get('capture', '')}   {status['updated']}   phase: {phase}"
    if progress.get("total"): head += f" {progress.get('done', 0)}/{progress['total']}"
    if progress.get("note"): head += f"  ({progress['note']})"
    lines.append(head)
    state = "complete" if status.get("inventory_finished") else "in progress"
    lines.append(f"passes {status['passes']} ({status['dispatches']} dispatches, {status['draws']} draws; inventory {state})")
    if status.get("draw_packets") and status["draws"] < status["draw_packets"]:
        lines.append(f"  WARNING: {status['draws']} of the replay's {status['draw_packets']} draw packets are in the inventory (no [draw-io] trace)")
    lines.append(f"  coverage     {bar(status['coverage_pct'])}  {status['covered']}/{status['passes']} ran with no skip/fallback/null binding")
    lines.append(f"  checked      {bar(status['checked_pct'])}  {status['checked']} of {status['passes']} passes run offline (" + ", ".join(f"{v['checked']}/{v['passes']} {k}es" if k == "dispatch" else f"{v['checked']}/{v['passes']} {k}s" for k, v in status["checked_by_kind"].items()) + ")")
    lines.append(f"  consistency  {bar(status['consistency_pct'])}  {status['consistent']}/{status['checked']} checked passes: offline == replay ({status['exact']} exact, {status['within_tolerance']} within float tolerance; {status.get('unstable', 0)} more unstable alone)")
    lines.append(f"  verified     {bar(status['verified_pct'])}  {status['ok']}/{status['judged']} judged passes ok")
    lines.append(f"  SCORE        {bar(status['score_pct'])}  proven {fmt(status['proven_pct'])}, possible {fmt(status['possible_pct'])}; only {status['checked']} of {status['passes']} passes checked offline")
    if status["issues_by_type"]:
        lines.append("  issues: " + ", ".join(f"{k} {v}" for k, v in sorted(status["issues_by_type"].items())) + f"; unattributed {status['unattributed']}; {status['downstream_of_bad']} passes read a bad pass's output")
    if status.get("known_bad"):
        lines.append(f"  known skips ({status['known_bad']} passes): " + "; ".join(status.get("known_failures", []))[:200])
    first = status.get("first_failure")
    if first:
        lines.append(f"first failing pass (#{first['index']} in frame order): {first['id']}")
        lines.append(f"  {first['reason'][:200]}")
    for failure in status["failures"][1:6]:
        lines.append(f"  then {failure['id']}: {failure['reason'][:120]}")
    for finding in status.get("findings", []):
        lines.append(f"finding {finding['pass']}: {finding['text'][:200]}")
    return "\n".join(lines)


def fmt(value) -> str:
    return "n/a" if value is None else f"{value:.2f}%"


def enable_ansi() -> None:
    if os.name == "nt": os.system("")


def redraw(text: str) -> None:
    sys.stdout.write("\x1b[H\x1b[J" + text + "\n")
    sys.stdout.flush()


# --------------------------------------------------------------------------------------------------------------------
# Replay runner
# --------------------------------------------------------------------------------------------------------------------

def default_work(capture: str) -> Path:
    return PS5RUN / "captures" / "fidelity" / Path(capture).name


def load_config(work: Path) -> dict:
    """config.json in the work dir: {"capture": DIR, "replay": EXE, "args": [...], "env": {...}, "path": [...]}."""
    config = load_json(work / "config.json", None)
    if config is None: raise SystemExit(f"no {work / 'config.json'}: write it first (see docs/dev/Fidelity.md)")
    return config


def free_commit_gib() -> float | None:
    if os.name != "nt": return None
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("totalPhys", ctypes.c_ulonglong), ("availPhys", ctypes.c_ulonglong),
                    ("totalPage", ctypes.c_ulonglong), ("availPage", ctypes.c_ulonglong), ("totalVirtual", ctypes.c_ulonglong),
                    ("availVirtual", ctypes.c_ulonglong), ("availExtended", ctypes.c_ulonglong)]
    status = Status()
    status.length = ctypes.sizeof(Status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return status.availPage / 2**30


def replay_running() -> bool:
    if os.name != "nt": return False
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq agc_frame_replay.exe", "/NH"], capture_output=True, text=True).stdout
    return "agc_frame_replay.exe" in output


def start_replay(work: Path, config: dict, extra_env: dict, extra_args: list[str], log: Path, name: str) -> subprocess.Popen:
    """The replay writes its dumps (shader_*.req, inputs_*, after_*) into its working directory: work/<name>."""
    if replay_running(): raise SystemExit("another agc_frame_replay is running: one replay at a time")
    free = free_commit_gib()
    need = float(config.get("commit_gib", 20))
    if free is not None and free < need: raise SystemExit(f"only {free:.1f} GiB commit free, the replay needs ~{need:.0f}")
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(config.get("path", [])) + os.pathsep + env.get("PATH", "")
    env.update({k: str(v) for k, v in config.get("env", {}).items()})
    env.update(extra_env)
    command = [config["replay"], config["capture"], *config.get("args", []), *extra_args]
    (work / "replay-command.txt").write_text(" ".join(command) + "\n" + "\n".join(f"{k}={v}" for k, v in {**config.get("env", {}), **extra_env}.items()) + "\n")
    run_dir = work / name
    run_dir.mkdir(parents=True, exist_ok=True)
    return subprocess.Popen(command, cwd=run_dir, env=env, stdout=open(work / "replay-stdout.log", "wb"), stderr=open(log, "wb"))


def follow(path: Path, process: subprocess.Popen | None, on_line, on_tick, tick=1.0) -> None:
    """Calls on_line for every line of a growing log until the process ends (or at EOF without a process)."""
    while not path.exists():
        if process is not None and process.poll() is not None: return
        time.sleep(0.2)
    with open(path, "r", encoding="utf-8", errors="replace") as file:
        partial = ""
        last = 0.0
        while True:
            chunk = file.readline()
            if chunk:
                partial += chunk
                if partial.endswith("\n"):
                    on_line(partial)
                    partial = ""
                    if time.monotonic() - last < tick: continue
            ended = process is None or process.poll() is not None
            if time.monotonic() - last >= tick or (not chunk and ended):
                last = time.monotonic()
                on_tick()
            if not chunk:
                if ended:
                    if partial: on_line(partial)
                    return
                time.sleep(0.2)


# --------------------------------------------------------------------------------------------------------------------
# Subcommands
# --------------------------------------------------------------------------------------------------------------------

def set_progress(work: Path, phase: str, **fields) -> None:
    write_json(work / "progress.json", {"phase": phase, "since": time.strftime("%Y-%m-%dT%H:%M:%S"), **fields})


def command_inventory(args) -> None:
    work = Path(args.dir)
    config = load_json(work / "config.json", {})
    inventory = Inventory(config.get("known_skips"))
    process = None
    if args.run:
        config = load_config(work)
        log = work / "inventory.log"
        env = {"APS5_TRACE_DISPATCH_IO": "1", "APS5_TRACE_DRAW_IO": "1", "APS5_TRACE_SKIPS": "1"}
        process = start_replay(work, config, env, ["--loop", "1"], log, "run-inventory")
        source = config["capture"]
    else:
        log = Path(args.log)
        source = args.capture or str(log)
    enable_ansi()
    started = time.monotonic()

    def tick() -> None:
        finished = process is None or process.poll() is not None
        save_inventory(work, inventory, source, finished and (inventory.stopped or process is None))
        note = f"{inventory.lines} log lines, {time.monotonic() - started:.0f} s"
        set_progress(work, "inventory" if not finished else "idle", done=len(inventory.commands), note=note)
        if not args.quiet: redraw(render(evaluate(work)))

    follow(log, process, inventory.feed, tick)
    inventory.finish()
    tick()
    if process is not None and process.returncode != 0:
        print(f"replay exited with {process.returncode}; see {log}")
    if args.quiet: print(render(evaluate(work)))


def command_report(args) -> None:
    work = Path(args.dir)
    enable_ansi()
    if not args.live:
        status = evaluate(work)
        print(json.dumps(status, indent=1) if args.json else render(status))
        return
    try:
        while True:
            redraw(render(evaluate(work)))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass


def command_verdict(args) -> None:
    work = Path(args.dir)
    verdicts = load_json(work / "verdicts.json", {})
    known = {p["id"] for p in load_json(work / "inventory.json", {"passes": []})["passes"]}
    if known and args.pass_id not in known: raise SystemExit(f"no pass {args.pass_id} in the inventory")
    if args.verdict == "unknown": verdicts.pop(args.pass_id, None)
    else: verdicts[args.pass_id] = {"verdict": args.verdict, "note": args.note or "", "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
    write_json(work / "verdicts.json", verdicts)
    print(render(evaluate(work)))


def pass_dir(work: Path, pass_id: str) -> Path:
    return work / "passes" / re.sub(r"[^0-9A-Za-z]+", "_", pass_id)


def capture_ranges(p: dict, max_bytes: int) -> tuple[list[tuple[int, int, str]], list[str]]:
    """A pass's ranges for APS5_CAPTURE_PASSES ('i' inputs, 'o' outputs, saved before and after), and why any is left
    out. Null bindings (address 0) are not ranges."""
    ranges: dict[int, list] = {}
    for kind, entries in (("i", p["inputs"]), ("o", p["outputs"])):
        for r in entries:
            if r["addr"] == 0 or r["bytes"] == 0: continue
            known = ranges.get(r["addr"])
            if known is None: ranges[r["addr"]] = [r["addr"], r["bytes"], kind]
            else:
                known[1] = max(known[1], r["bytes"])
                if kind == "o": known[2] = "o"
    kept, left = [], []
    for address, size, kind in sorted(ranges.values()):
        if size > max_bytes: left.append(f"0x{address:x}+0x{size:x} over {max_bytes >> 20} MiB")
        else: kept.append((address, size, kind))
    return kept, left


def checkable(p: dict) -> bool:
    """A pass the harness can capture and run alone: a dispatch that reached the device with outputs, or a render pass
    with at least one drawn draw (and serials from a [draw-io] trace with "draw N")."""
    if any(i["type"] in ("known-skip", "no-bindings") for i in p["issues"]): return False
    if p["kind"] == "dispatch":
        return p["queue"] >= 0 and p["traced"] and bool(p["outputs"]) and not any(i["type"] == "skip" for i in p["issues"])
    return bool(p.get("drawn")) and min(p.get("serials", [-1])) >= 0


def run_capture(work: Path, config: dict, inventory: dict, wanted: set | None, budget_gib: float, max_range_mib: float, again: bool, quiet: bool, limit: int = 0) -> int:
    """One replay capturing the chosen passes (default: every checkable pass not checked yet, in frame order); the
    driver stops starting passes once `budget_gib` of new data is written. Returns how many passes finished."""
    checks = load_checks(work)
    passes = [p for p in inventory["passes"] if checkable(p) and (wanted is None or p["id"] in wanted)]
    if wanted is not None and len(passes) < len(wanted):
        print(f"not capturable (refused, untraced or without outputs): {', '.join(sorted(wanted - {p['id'] for p in passes}))}")
    if wanted is None and not again: passes = [p for p in passes if checks.get(p["id"], {}).get("result") not in ("match", "near", "mismatch", "unstable")]
    if limit: passes = passes[:limit]
    if not passes: return 0
    max_bytes = int(max_range_mib * 2**20)
    lines, planned = [], {}
    for p in passes:
        directory = pass_dir(work, p["id"])
        if directory.exists(): shutil.rmtree(directory, ignore_errors=True)
        if directory.exists():
            # A file in it is still open (a stuck runner): set the old data aside, or capture into a new directory.
            try:
                directory.rename(directory.with_name(f"{directory.name}.stale-{time.strftime('%H%M%S')}"))
            except OSError:
                directory = directory.with_name(f"{directory.name}_{time.strftime('%H%M%S')}")
        if p["kind"] == "dispatch":
            ranges, _ = capture_ranges(p, max_bytes)
            lines.append(f"{p['program']:x} {p['instance']} {directory} " + ",".join(f"{a:x}:{s:x}:{k}" for a, s, k in ranges))
        else:
            lines.append(f"draws {p['serials'][0]} {p['drawn'][-1]} {directory}")
        planned[p["id"]] = {"dir": str(directory), "kind": p["kind"]}
    free = shutil.disk_usage(work).free
    budget = min(int(budget_gib * 2**30), free - 5 * 2**30)
    if budget <= 0: raise SystemExit(f"only {free / 2**30:.1f} GiB free on the disk (5 GiB are kept free)")
    (work / "capture-passes.txt").write_text("\n".join(lines) + "\n")
    captured = load_json(work / "captured.json", {})
    captured.update(planned)
    write_json(work / "captured.json", captured)
    print(f"capturing up to {len(planned)} passes in frame order, budget {budget / 2**30:.2f} GiB")
    log = work / "capture.log"
    env = {"APS5_CAPTURE_PASSES": str(work / "capture-passes.txt"), "APS5_CAPTURE_PASSES_BUDGET_MIB": str(budget >> 20)}
    process = start_replay(work, config, env, ["--loop", "1"], log, "run-capture")
    done = 0
    started = time.monotonic()

    def line(text: str) -> None:
        nonlocal done
        if text.startswith("[capture-pass]") and ": done," in text: done += 1

    def tick() -> None:
        set_progress(work, "capture", done=done, total=len(planned), note=f"{time.monotonic() - started:.0f} s")
        if not quiet: redraw(render(evaluate(work)))

    enable_ansi()
    follow(log, process, line, tick)
    set_progress(work, "idle", done=done, total=len(planned), note=f"capture finished, exit {process.returncode}")
    print(f"captured {done} of {len(planned)} passes; replay exit {process.returncode}")
    return done


def command_capture(args) -> None:
    work = Path(args.dir)
    inventory = load_json(work / "inventory.json", None)
    if inventory is None or not inventory.get("finished"): raise SystemExit("run a complete inventory first")
    wanted = set(args.passes.split(",")) if args.passes else None
    run_capture(work, load_config(work), inventory, wanted, args.budget_gib, args.max_range_mib, args.again, args.quiet, args.limit)
    print(render(evaluate(work)))


def read_dispatch(directory: Path) -> dict:
    fields = {}
    for text in (directory / "dispatch.txt").read_text().splitlines():
        key, _, value = text.partition(" ")
        fields[key] = value
    return fields


def range_file(directory: Path, address: int, prefix: str = "") -> Path | None:
    for name in (f"{prefix}{address:x}.bin", f"{address:x}.chunks"):
        if (directory / name).exists(): return directory / name
    return None


def read_manifest(path: Path) -> tuple[int, Path, list[str]]:
    lines = path.read_text().splitlines()
    _, size, _, blobs = lines[0].split(" ", 3)
    return int(size, 16), Path(blobs), lines[1:]


def read_ranges(directory: Path) -> list[tuple[int, int, str]]:
    ranges = []
    for text in (directory / "ranges.txt").read_text().splitlines():
        address, size, kind = text.split()
        ranges.append((int(address, 16), int(size, 16), kind))
    return ranges


def tool_path(config: dict, key: str, name: str) -> Path:
    # Next to the replay (libs/unpatched) or in the driver's build directory.
    replay = Path(config["replay"])
    for candidate in (config.get(key), replay.parent / name, replay.parents[2] / "prx" / "libSceAgcDriver" / name):
        if candidate and Path(candidate).exists(): return Path(candidate)
    return replay.parent / name


def run_tool(config: dict, command: list[str], cwd: Path, timeout: int = 300) -> subprocess.CompletedProcess | str:
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(config.get("path", [])) + os.pathsep + env.get("PATH", "")
    env.update({k: str(v) for k, v in config.get("env", {}).items() if k != "APS5_SKIP_PROGRAMS"})
    try:
        run = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"{Path(command[0]).stem} timed out"
    (cwd / "run.log").write_text(run.stdout + run.stderr, encoding="utf-8", errors="replace")
    if run.returncode != 0: return f"{Path(command[0]).stem}: " + ((run.stderr.strip() or run.stdout.strip()).splitlines() or [f"exit {run.returncode}"])[-1][:200]
    return run


def check_pass(work: Path, config: dict, p: dict, entry: dict, ulps: float) -> dict:
    """Runs a captured pass alone and compares each output with what the replay left in it: a dispatch through
    agc_dispatch_run (its request recompiled and dispatched directly), a render pass through agc_pass_run (its
    packets through the whole driver in a fresh process). A mismatch is run alone a second time: when the two runs
    alone differ from each other the pass is "unstable" (its output depends on GPU scheduling, as an atomic append
    order does), not a replay difference."""
    directory = Path(entry["dir"])
    result = {"pass": p["id"], "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
    done = directory / "done.txt"
    if not done.exists() or "over_budget 1" in done.read_text():
        return {**result, "result": "incomplete", "detail": "the capture did not finish the pass (budget, a refused last draw, or the replay stopped)"}
    outputs = [(a, s) for a, s, k in read_ranges(directory) if k == "o" and range_file(directory / "after", a) is not None]
    if not outputs: return {**result, "result": "error", "detail": "no output range was saved"}
    blobs = str(work / "capture-passes.txt.blobs")

    def run_alone(name: str):
        """Returns a function from address to the run's output file, or an error text."""
        out = directory / name
        if out.exists(): shutil.rmtree(out, ignore_errors=True)
        out.mkdir(exist_ok=True)
        # A dispatch runs through agc_dispatch_run (its request alone); a thread-count indirect one, which that tool
        # cannot launch, runs its saved packet through the whole driver (agc_pass_run) as a render pass does.
        direct = p["kind"] == "dispatch" and read_dispatch(directory).get("thread_indirect") != "1"
        if direct:
            dispatch = read_dispatch(directory)
            if not (directory / "shader.req").exists(): return "no request was saved"
            command = [str(tool_path(config, "dispatch_run", "agc_dispatch_run.exe")), str(directory / "shader.req"), "--groups", dispatch["groups"], "--inputs", str(directory / "before"), "--blobs", blobs]
            for address, size in outputs: command += ["--dump", f"{address:x}:{size:x}"]
            run = run_tool(config, command, out)
            produced = lambda a: range_file(out, a, "dispatch_") or out / f"dispatch_{a:x}.bin"
        else:
            run = run_tool(config, [str(tool_path(config, "pass_run", "agc_pass_run.exe")), str(directory), "--out", str(out), "--blobs", blobs], out)
            produced = lambda a: range_file(out, a) or out / f"{a:x}.bin"
        if isinstance(run, str): return run
        # A run alone that read memory the capture did not save is the capture's gap, not the driver's difference.
        if (missing := re.search(r"guest memory is not readable at 0x[0-9a-f]+ \(range (0x[0-9a-f]+\+0x[0-9a-f]+)\)", run.stdout + run.stderr)):
            return f"capture incomplete: the run alone read {missing.group(1)}, which the capture did not save"
        # The run alone refused the pass (the driver skipped it there): nothing to compare.
        if (refused := re.search(r"\[gpu\] skipped (?:dispatch|draw).*|\[draw\] pipelined commit failed.*", run.stdout + run.stderr)):
            return f"the run alone refused the pass: {refused.group(0)[:240]}"
        # The runner's restored memory counted as CPU writes over results still on the GPU: the run alone dropped them.
        if "keeping the CPU's blocks" in run.stdout + run.stderr:
            return "the run alone kept CPU-written blocks over its GPU results (restored memory taken for CPU writes); not compared"
        return produced

    direct = p["kind"] == "dispatch" and read_dispatch(directory).get("thread_indirect") != "1"
    first = run_alone("offline" if direct else "isolated")
    if isinstance(first, str): return {**result, "result": "error", "detail": first}
    before = lambda a: range_file(directory / "before", a) or directory / "before" / f"{a:x}.bin"
    compared = compare_outputs(config, result, image_ranges(p), [(a, s, range_file(directory / "after", a), first(a), before(a)) for a, s in outputs], ulps)
    if compared["result"] != "mismatch": return compared
    # Alone, the pass left every output as it was while the replay changed them: the runner did not run it (a draw
    # without targets that writes storage images does not write alone yet), not a replay difference. The opposite case
    # (the replay wrote nothing, as the fog injection) stays a mismatch.
    changes = re.findall(r"replay changed (\d+), the run alone changed (\d+)", compared.get("detail", ""))
    if changes and all(int(alone) == 0 and int(replay) > 0 for replay, alone in changes):
        return {**compared, "result": "error", "detail": "the run alone wrote nothing where the replay wrote: " + compared["detail"][:400]}
    second = run_alone("offline2" if direct else "isolated2")
    if isinstance(second, str): return compared
    again = compare_outputs(config, result, image_ranges(p), [(a, s, first(a), second(a), before(a)) for a, s in outputs], ulps)
    if again["result"] == "mismatch":
        return {**compared, "result": "unstable", "detail": f"two runs alone differ ({again['detail'][:300]}); replay vs alone: {compared['detail'][:400]}"}
    return compared


def prune(work: Path, results: list[dict]) -> int:
    """Removes the captured data of passes that matched (their check result stays) and the blobs nothing links."""
    freed = 0
    for entry in results:
        if entry.get("result") not in ("match", "near"): continue
        directory = Path(load_json(work / "captured.json", {}).get(entry["pass"], {}).get("dir", pass_dir(work, entry["pass"])))
        if directory.exists(): shutil.rmtree(directory, ignore_errors=True)
    blobs = work / "capture-passes.txt.blobs"
    referenced = set()
    for manifest in (work / "passes").glob("*/*/*.chunks"):
        try:
            referenced.update(read_manifest(manifest)[2])
        except (OSError, ValueError):
            pass
    if blobs.exists():
        for blob in blobs.iterdir():
            try:
                if blob.name not in referenced and os.stat(blob).st_nlink <= 1:
                    freed += blob.stat().st_size
                    blob.unlink()
            except OSError:
                pass
    return freed


def run_checks(work: Path, config: dict, wanted: set | None, again: bool, ulps: float, quiet: bool) -> list[dict]:
    captured = load_json(work / "captured.json", {})
    if wanted: captured = {k: v for k, v in captured.items() if k in wanted}
    checks = load_checks(work)
    passes = {p["id"]: p for p in load_json(work / "inventory.json", {"passes": []})["passes"]}
    todo = [k for k in captured if again or checks.get(k, {}).get("result") not in ("match", "near", "mismatch", "unstable")]
    results = []
    enable_ansi()
    with open(work / "checks.jsonl", "a", encoding="utf-8") as out:
        for done, pass_id in enumerate(todo):
            set_progress(work, "check", done=done, total=len(todo), note=pass_id)
            if not quiet: redraw(render(evaluate(work)))
            entry = check_pass(work, config, passes[pass_id], captured[pass_id], ulps) if pass_id in passes else {"pass": pass_id, "result": "error", "detail": "not in the inventory"}
            out.write(json.dumps(entry) + "\n")
            out.flush()
            results.append(entry)
            if quiet and entry["result"] != "match": print(f"{pass_id}: {entry['result']} {entry.get('detail', '')[:160]}")
    set_progress(work, "idle", done=len(todo), total=len(todo), note="check finished")
    return results


def command_check(args) -> None:
    work = Path(args.dir)
    results = run_checks(work, load_config(work), set(args.passes.split(",")) if args.passes else None, args.again, args.ulps, args.quiet)
    if args.prune: print(f"pruned matched passes: {prune(work, results) / 2**30:.2f} GiB of blobs freed")
    print(render(evaluate(work)))


def command_sweep(args) -> None:
    """Rounds of capture (frame order, within the budget) -> check -> prune, until every checkable pass is checked or
    the rounds run out: one replay per round."""
    work = Path(args.dir)
    config = load_config(work)
    inventory = load_json(work / "inventory.json", None)
    if inventory is None or not inventory.get("finished"): raise SystemExit("run a complete inventory first")
    for round_number in range(args.rounds):
        set_progress(work, "sweep", done=round_number, total=args.rounds, note="capture")
        if run_capture(work, config, inventory, None, args.budget_gib, args.max_range_mib, False, args.quiet, args.limit) == 0:
            print("every checkable pass is checked")
            break
        results = run_checks(work, config, None, False, args.ulps, args.quiet)
        print(f"round {round_number + 1}: checked {len(results)}; pruned {prune(work, results) / 2**30:.2f} GiB")
        print(render(evaluate(work)))


def image_ranges(p: dict) -> dict[int, dict]:
    """The pass's image bindings by address (layout and format for agc_surface_png)."""
    return {r["addr"]: r for r in p["outputs"] + p["inputs"] if "w" in r}


def compare_outputs(config: dict, result: dict, images: dict[int, dict], outputs: list, ulps: float) -> dict:
    """Compares each (address, size, replay dump, isolated-run dump, input state) output. Equal bytes are exact; an image
    that differs is compared per element by agc_surface_png --compare within `ulps` steps of its format's precision; a
    buffer that differs counts as near only when every differing 32-bit word is a finite float on both sides within
    the float32 tolerance (labelled "as float32"). The result is match (all exact), near or mismatch."""
    details, state = [], "match"
    for address, size, replay_path, mine_path, before_path in outputs:
        if not mine_path.exists():
            details.append(f"0x{address:x}: the isolated run left no dump")
            state = "mismatch"
            continue
        if replay_path.suffix == ".chunks" or mine_path.suffix == ".chunks":
            how, near_only = compare_chunked(replay_path, mine_path, before_path, ulps)
            if how is None: continue
            details.append(f"0x{address:x}+0x{size:x}: {how}")
            if not near_only: state = "mismatch"
            elif state == "match": state = "near"
            continue
        replay, mine = replay_path.read_bytes(), mine_path.read_bytes()
        if replay == mine: continue
        before = before_path.read_bytes() if before_path.exists() else b""
        changes = f"replay changed {count_differences(before, replay)}, the run alone changed {count_differences(before, mine)}"
        image = images.get(address)
        verdict, how = "beyond", ""
        if image is not None:
            compared = surface_compare(config, image, replay_path, mine_path, ulps)
            if compared is not None:
                verdict = "near" if compared["beyond"] == 0 else "beyond"
                how = f"{compared['beyond']} of {compared['elements']} elements beyond {ulps:g} ulps, {compared['near']} near, max abs {compared['max_abs']}" + (f", first at {compared['first_beyond']}" if compared["beyond"] else "")
        elif len(replay) == len(mine) and len(replay) % 4 == 0:
            near, beyond = float_words_compare(replay, mine, ulps)
            if beyond == 0:
                verdict, how = "near", f"{near} words within {ulps:g} float32 ulps (as float32)"
        if not how: how = f"{count_differences(replay, mine)} bytes differ"
        details.append(f"0x{address:x}+0x{size:x}: {how}; {changes}")
        if verdict == "beyond": state = "mismatch"
        elif state == "match": state = "near"
    return {**result, "result": state, "outputs": len(outputs), "detail": "; ".join(details)[:800]}


def chunk_list(path: Path) -> tuple[Path, list[str]]:
    """A range file as (blob directory, chunk blob names); a plain .bin is one chunk."""
    if path.suffix == ".chunks":
        _, blobs, names = read_manifest(path)
        return blobs, names
    return path.parent, [path.name]


def compare_chunked(replay_path: Path, mine_path: Path, before_path: Path, ulps: float) -> tuple[str | None, bool]:
    """Compares two large ranges chunk by chunk: equal chunk names are equal content; only differing chunks are read.
    Returns (None, True) when equal, else (detail, whether every difference is within the float32 tolerance)."""
    replay_dir, replay_chunks = chunk_list(replay_path)
    mine_dir, mine_chunks = chunk_list(mine_path)
    if replay_path.suffix != mine_path.suffix: return f"saved in different forms ({replay_path.name} vs {mine_path.name})", False
    if replay_chunks == mine_chunks: return None, True
    differ = near = beyond = chunks = 0
    for a, b in zip(replay_chunks, mine_chunks):
        if a == b: continue
        chunks += 1
        x, y = (replay_dir / a).read_bytes(), (mine_dir / b).read_bytes()
        differ += count_differences(x, y)
        n, m = float_words_compare(x, y, ulps) if len(x) == len(y) and len(x) % 4 == 0 else (0, 1)
        near += n
        beyond += m
    if len(replay_chunks) != len(mine_chunks): beyond += 1
    detail = f"{differ} bytes differ in {chunks} of {len(replay_chunks)} MiB chunks" + (f" ({near} words within {ulps:g} float32 ulps)" if beyond == 0 else "")
    return detail, beyond == 0


def surface_compare(config: dict, image: dict, a: Path, b: Path, ulps: float) -> dict | None:
    tool = Path(config.get("surface_png", Path(config["replay"]).parents[2] / "prx" / "libSceAgcDriver" / "agc_surface_png.exe"))
    out = a.parent.parent / "compare" / f"{a.stem}.png"
    out.parent.mkdir(exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(config.get("path", [])) + os.pathsep + env.get("PATH", "")
    run = subprocess.run([str(tool), str(a), str(out), *surface_args(image, image["kind"]), "--compare", str(b), "--ulps", str(ulps)], env=env, capture_output=True, text=True)
    match = re.search(r"compare: elements (\d+) exact (\d+) near (\d+) beyond (\d+) max_abs (\S+) ulps \S+ first_beyond (\S+)", run.stdout)
    if match is None: return None
    return {"elements": int(match[1]), "exact": int(match[2]), "near": int(match[3]), "beyond": int(match[4]), "max_abs": match[5], "first_beyond": match[6], "png": str(out)}


def float_words_compare(a: bytes, b: bytes, ulps: float) -> tuple[int, int]:
    import math
    import struct
    near = beyond = 0
    for start in range(0, len(a), 4096):
        x, y = a[start:start + 4096], b[start:start + 4096]
        if x == y: continue
        for (p,), (q,) in zip(struct.iter_unpack("<f", x), struct.iter_unpack("<f", y)):
            if p == q: continue
            if math.isfinite(p) and math.isfinite(q) and abs(p - q) <= ulps * 2.0 ** -23 * max(abs(p), abs(q), 2.0 ** -126): near += 1
            else: beyond += 1
    return near, beyond


def count_differences(a: bytes, b: bytes) -> int:
    if len(a) != len(b): return max(len(a), len(b))
    if a == b: return 0
    # Equal 4 KiB chunks are skipped whole (no numpy in the tools Python).
    differ = 0
    for start in range(0, len(a), 4096):
        x, y = a[start:start + 4096], b[start:start + 4096]
        if x != y: differ += sum(p != q for p, q in zip(x, y))
    return differ


def surface_args(r: dict, kind: str) -> list[str]:
    """agc_surface_png's layout arguments for an inventory range: a color target's VkFormat and ColorTileMode
    ([draw-io]), or a texture/storage image's guest format and TextureTileMode ([dispatch-io])."""
    size = f"{r['w']}x{r['h']}" + (f"x{r['d']}" + ("v" if r.get("volume") else "") if r.get("d", 1) > 1 or r.get("volume") else "")
    if kind == "color": return ["--size", size, "--vk-format", str(r["fmt"]), "--color-tile", str(r["tile"])]
    return ["--size", size, "--format", str(r["fmt"]), "--tile", str(r["tile"])]


def surface_png(config: dict, image: dict, source: Path, out: Path, extra: list[str]) -> tuple[bool, str]:
    run = subprocess.run([str(tool_path(config, "surface_png", "agc_surface_png.exe")), str(source), str(out), *surface_args(image, image["kind"]), *extra],
                         env=tool_env(config), capture_output=True, text=True)
    return run.returncode == 0, (run.stdout or run.stderr).strip()


def tool_env(config: dict) -> dict:
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(config.get("path", [])) + os.pathsep + env.get("PATH", "")
    return env


def next_to_judge(work: Path, inventory: dict) -> str | None:
    """The next pass to look at: a checked mismatch without a verdict, else any captured pass without one (frame order)."""
    checks = load_checks(work)
    verdicts = load_json(work / "verdicts.json", {})
    open_passes = [p for p in inventory["passes"] if p["id"] not in verdicts and pass_dir(work, p["id"]).exists()]
    for p in open_passes:
        if checks.get(p["id"], {}).get("result") == "mismatch": return p["id"]
    return open_passes[0]["id"] if open_passes else None


def command_view(args) -> None:
    """Shows a pass's images in the terminal (sixel, or half blocks): for an output where the replay and the run alone
    differ, replay | alone | difference; otherwise input | replay. --ask then takes the verdict."""
    import termimage
    work = Path(args.dir)
    config = load_config(work)
    inventory = load_json(work / "inventory.json", {"passes": []})
    pass_id = next_to_judge(work, inventory) if args.pass_id == "next" else args.pass_id
    if pass_id is None: raise SystemExit("no captured pass is waiting for a verdict")
    found = [p for p in inventory["passes"] if p["id"] == pass_id]
    if not found: raise SystemExit(f"no pass {pass_id}")
    p = found[0]
    images = [r for r in p["outputs"] + (p["inputs"] if args.inputs else []) if "w" in r and r["addr"] != 0 and r["kind"] != "depth" and r["w"] > 0]
    if args.address: images = [r for r in images if r["addr"] == int(args.address, 16)]
    directory = pass_dir(work, p["id"])
    views = work / "views"
    views.mkdir(exist_ok=True)
    check = load_checks(work).get(p["id"], {})
    verdict = load_json(work / "verdicts.json", {}).get(p["id"], {})
    enable_ansi()
    print(f"{p['id']} (#{p['index']} in frame order, {p['kind']}{', %d draws' % p['draws'] if p['kind'] == 'draw' else ''}): check {check.get('result', 'not run')}"
          + (f", verdict {verdict['verdict']}" if verdict else ""))
    if check.get("detail"): print(f"  {check['detail'][:300]}")
    for issue in p["issues"][:3]: print(f"  issue {issue['type']}: {issue['text'][:160]}")
    shown = 0
    slice_args = ["--slice", str(args.slice)] if args.slice is not None else []
    for r in images:
        replay = range_file(directory / "after", r["addr"])
        alone = range_file(directory / "isolated", r["addr"]) or range_file(directory / "offline", r["addr"], "dispatch_")
        before = range_file(directory / "before", r["addr"])
        stem = views / f"{re.sub(r'[^0-9A-Za-z]+', '_', p['id'])}_{r['addr']:x}"
        size = f"{r['w']}x{r['h']}" + (f"x{r['d']}" if r.get("d", 1) > 1 else "")
        if replay is not None and alone is not None and replay.suffix == ".bin" and alone.suffix == ".bin" and replay.read_bytes() != alone.read_bytes():
            out = stem.with_name(stem.name + "_compare.png")
            ok, text = surface_png(config, r, replay, out, ["--compare", str(alone), *slice_args])
            title = f"{r['kind']} 0x{r['addr']:x} {size}: replay | {'isolated' if 'isolated' in str(alone) else 'offline'} | difference (yellow within tolerance, red beyond)"
        elif replay is not None and replay.suffix == ".bin":
            out = stem.with_name(stem.name + "_replay.png")
            ok, text = surface_png(config, r, replay, out, slice_args)
            title = f"{r['kind']} 0x{r['addr']:x} {size}: replay" + ("" if alone is None else " (the run alone matches)")
            if ok and before is not None and before.suffix == ".bin" and args.inputs:
                prior = stem.with_name(stem.name + "_before.png")
                if surface_png(config, r, before, prior, slice_args)[0]:
                    termimage.save_side_by_side([str(prior), str(out)], str(out))
                    title = f"{r['kind']} 0x{r['addr']:x} {size}: before | replay"
        elif before is not None and before.suffix == ".bin":
            out = stem.with_name(stem.name + "_before.png")
            ok, text = surface_png(config, r, before, out, slice_args)
            title = f"{r['kind']} 0x{r['addr']:x} {size}: input"
        else:
            continue
        if not ok:
            print(f"  {title}: {text[:200]}")
            continue
        print(f"\n{title}\n  {out}")
        compare = re.search(r"compare: .*", text)
        if compare: print(f"  {compare.group(0)}")
        if args.mode != "png": print(termimage.show(str(out), args.mode))
        shown += 1
    if shown == 0: print(f"nothing saved for {p['id']} yet: run `capture --passes {p['id']}` (and `check`) first")
    if args.ask and shown:
        answer = input("verdict? [o]k / [w]rong / [s]kip: ").strip().lower()
        if answer[:1] in ("o", "w"):
            note = input("note (optional): ").strip()
            verdicts = load_json(work / "verdicts.json", {})
            verdicts[p["id"]] = {"verdict": "ok" if answer[:1] == "o" else "wrong", "note": note, "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
            write_json(work / "verdicts.json", verdicts)
            print(render(evaluate(work)))


def command_finding(args) -> None:
    """findings.json: leads a person or a session wrote down for a pass (shown in the report and status.json)."""
    work = Path(args.dir)
    findings = [f for f in load_json(work / "findings.json", []) if not (args.remove and f["pass"] == args.pass_id)]
    if not args.remove:
        findings.append({"pass": args.pass_id, "text": args.text, "time": time.strftime("%Y-%m-%dT%H:%M:%S")})
    write_json(work / "findings.json", findings)
    print(render(evaluate(work)))


def main() -> None:
    # Block characters and sixel go out as UTF-8 whatever the console code page (or a redirect) says.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", help="work dir (default ps5run/captures/fidelity/<capture>)")
    parser.add_argument("--capture", help="capture name or dir, for the default work dir")
    sub = parser.add_subparsers(dest="command", required=True)
    inventory = sub.add_parser("inventory")
    inventory.add_argument("--log", help="an existing traced replay log")
    inventory.add_argument("--run", action="store_true", help="run the replay from config.json with the traces on")
    inventory.add_argument("--quiet", action="store_true", help="no live view, only the final report")
    report = sub.add_parser("report")
    report.add_argument("--live", action="store_true")
    report.add_argument("--interval", type=float, default=1.0)
    report.add_argument("--json", action="store_true")
    capture = sub.add_parser("capture", help="capture chosen compute passes' inputs and replay outputs (one replay)")
    capture.add_argument("--passes", help="comma-separated pass ids (default: every checkable pass not checked yet, in frame order)")
    capture.add_argument("--limit", type=int, default=0, help="at most this many passes")
    capture.add_argument("--budget-gib", type=float, default=6.0, help="disk the run may write")
    capture.add_argument("--max-range-mib", type=float, default=4096.0, help="a listed dispatch range bigger than this is left out")
    capture.add_argument("--again", action="store_true", help="also passes captured before")
    capture.add_argument("--quiet", action="store_true")
    check = sub.add_parser("check", help="run captured passes offline and compare with the replay's outputs")
    check.add_argument("--passes")
    check.add_argument("--ulps", type=float, default=4.0, help="float tolerance: steps of each format's precision (near, reported apart from exact)")
    check.add_argument("--again", action="store_true")
    check.add_argument("--quiet", action="store_true")
    check.add_argument("--prune", action="store_true", help="remove the captured data of passes that matched")
    sweep = sub.add_parser("sweep", help="rounds of capture -> check -> prune in frame order (one replay per round)")
    sweep.add_argument("--rounds", type=int, default=1)
    sweep.add_argument("--limit", type=int, default=0, help="at most this many passes per round")
    sweep.add_argument("--budget-gib", type=float, default=6.0, help="new captured data per round")
    sweep.add_argument("--max-range-mib", type=float, default=4096.0)
    sweep.add_argument("--ulps", type=float, default=4.0)
    sweep.add_argument("--quiet", action="store_true")
    view = sub.add_parser("view", help="a captured pass's images in the terminal (replay | run alone | difference for a mismatch)")
    view.add_argument("pass_id", help="a pass id, or 'next': the next mismatch (else captured pass) without a verdict")
    view.add_argument("--address", help="only this image (hex)")
    view.add_argument("--slice", type=int, help="one array layer or volume slice instead of the grid")
    view.add_argument("--inputs", action="store_true", help="also the pass's input images (before | replay for outputs)")
    view.add_argument("--mode", choices=["auto", "sixel", "blocks", "png"], default="auto", help="auto: sixel in Windows Terminal, else half blocks; png: files only")
    view.add_argument("--ask", action="store_true", help="ask for the verdict after showing the images")
    finding = sub.add_parser("finding", help="record a lead for a pass (or --remove the pass's leads)")
    finding.add_argument("pass_id")
    finding.add_argument("text", nargs="?", default="")
    finding.add_argument("--remove", action="store_true")
    verdict = sub.add_parser("verdict")
    verdict.add_argument("pass_id")
    verdict.add_argument("verdict", choices=["ok", "wrong", "unknown"])
    verdict.add_argument("--note")
    args = parser.parse_args()
    if args.dir is None:
        if args.capture is None: raise SystemExit("give --dir or --capture")
        args.dir = str(default_work(args.capture))
    Path(args.dir).mkdir(parents=True, exist_ok=True)
    if args.command == "inventory":
        if not args.log and not args.run: raise SystemExit("inventory needs --log FILE or --run")
        command_inventory(args)
    elif args.command == "report":
        command_report(args)
    elif args.command == "verdict":
        command_verdict(args)
    elif args.command == "finding":
        command_finding(args)
    elif args.command == "view":
        command_view(args)
    elif args.command == "capture":
        command_capture(args)
    elif args.command == "sweep":
        command_sweep(args)
    elif args.command == "check":
        command_check(args)


if __name__ == "__main__":
    main()
