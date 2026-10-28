#!/usr/bin/env python3
"""Inject opt-in syscall *timestamps* into the three pinned holetest launches.

Only for diagnostic CI. The original upstream test is saved and restored by
run.sh; the unchanged pinned holetest binary still executes all 9 subcases.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import shlex

LINE = re.compile(r"^(\$here/src/holetest -f -w \$SCRATCH_MNT/testfile (1|16|256))$")


def instrument(original: str, output: pathlib.Path) -> str:
    targets: list[str] = []
    result = []
    for line in original.splitlines(keepends=True):
        m = LINE.fullmatch(line.rstrip("\n"))
        if not m:
            result.append(line)
            continue
        size = m.group(2)
        targets.append(size)
        trace = shlex.quote(str(output / f"holetest-{size}MiB.syscalls"))
        # -f follows the two worker threads. Trace only lifecycle syscalls,
        # never all ~65k pwrite calls; preserve behavior and keep overhead small.
        result.append(
            "strace -f -ttt -s 256 -o " + trace +
            " -e trace=openat,unlink,unlinkat,ftruncate,fallocate,mmap,munmap,close " + m.group(1) + "\n"
        )
    if targets != ["1", "16", "256"]:
        raise ValueError(f"pinned generic/346 command sequence changed: {targets!r}")
    return "".join(result)


# Each pinned holetest invocation creates and then unlinks testfile three
# times, in source order: zero-initialized, posix_fallocate, ftruncate.
# Epoch timestamps from strace -f -ttt preserve the actual launch timings.
# A timed-out subcase has a START without the matching successful unlink.
EPOCH = re.compile(r"(?<![\d.])(\d{10}\.\d+)\s+((?:openat|unlink)\(.*)$")
CASE_NAMES = ("zero-filled", "posix_fallocate", "ftruncate")


def phase_times(trace: str) -> list[dict[str, object]]:
    phases: list[dict[str, object]] = []
    active: dict[str, object] | None = None
    for line in trace.splitlines():
        match = EPOCH.search(line)
        if not match:
            continue
        stamp = float(match.group(1))
        call = match.group(2)
        if "testfile" not in call:
            continue
        if call.startswith("openat(") and "O_CREAT" in call and "O_EXCL" in call and "= -1" not in call:
            if active is not None:
                raise ValueError("previous holetest subcase never unlinked its file")
            if len(phases) >= len(CASE_NAMES):
                raise ValueError("unexpected extra holetest subcase")
            active = {"phase": CASE_NAMES[len(phases)], "start": stamp, "end": None, "seconds": None}
        elif call.startswith("unlink(") and "= 0" in call:
            if active is None:
                raise ValueError("holetest unlink without preceding create")
            active["end"] = stamp
            active["seconds"] = round(stamp - float(active["start"]), 3)
            phases.append(active)
            active = None
    if active is not None:
        phases.append(active)
    return phases


def analyze(directory: pathlib.Path) -> dict[str, list[dict[str, object]]]:
    results = {}
    for size in (1, 16, 256):
        file = directory / f"holetest-{size}MiB.syscalls"
        if not file.exists():
            continue
        results[str(size)] = phase_times(file.read_text())
    return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path)
    parser.add_argument("--original", type=pathlib.Path)
    parser.add_argument("--trace-dir", type=pathlib.Path, required=True)
    parser.add_argument("--summarize", action="store_true")
    args = parser.parse_args()
    if args.summarize:
        print(json.dumps(analyze(args.trace_dir), indent=2, sort_keys=True))
        return
    if args.script is None or args.original is None:
        parser.error("--script and --original required unless --summarize")
    args.original.write_bytes(args.script.read_bytes())
    args.script.write_text(instrument(args.original.read_text(), args.trace_dir))


if __name__ == "__main__":
    main()
