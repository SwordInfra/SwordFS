#!/usr/bin/env python3
"""Summarize pinned generic/346 diagnostics for the integrated Slice Chunk.

Raw Redis command counts and FUSE logs are evidence, not an assertion that a
command is uniquely attributable to a particular Slice publication.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re

from trace_generic346 import analyze

COMMAND = re.compile(r"^cmdstat_([^:]+):calls=(\d+),")
TOTAL = re.compile(r"^total_commands_processed:(\d+)")


def redis_counts(path: pathlib.Path, pattern: re.Pattern[str]) -> dict[str, int]:
    if not path.exists():
        return {}
    counts = {}
    for line in path.read_text().splitlines():
        match = pattern.match(line)
        if match:
            counts[match.group(1) if pattern is COMMAND else "total"] = int(
                match.group(2) if pattern is COMMAND else match.group(1)
            )
    return counts


def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int] | None:
    if not before or not after:
        return None
    return {name: after[name] - before.get(name, 0) for name in after}


def run_arm(directory: pathlib.Path) -> dict:
    raw = directory / "raw"
    try:
        env = json.loads((directory / "environment.json").read_text())
    except (OSError, ValueError):
        env = {}
    before = redis_counts(raw / "redis-commandstats-before.txt", COMMAND)
    after = redis_counts(raw / "redis-commandstats-after.txt", COMMAND)
    total_before = redis_counts(raw / "redis-stats-before.txt", TOTAL)
    total_after = redis_counts(raw / "redis-stats-after.txt", TOTAL)
    usage = {}
    for role in ("test", "scratch"):
        try:
            usage[role] = json.loads((raw / f"s3-{role}-volume-usage.json").read_text())
        except (OSError, ValueError):
            usage[role] = None
    writeback_errors = []
    for role in ("test", "scratch"):
        try:
            for line in (directory / f"swordfs-{role}.log").read_text().splitlines():
                if "FileReadWriter::Flush chunk FAILED" in line or "local RELEASE failed" in line:
                    writeback_errors.append({"role": role, "message": line})
        except OSError:
            writeback_errors.append({"role": role, "message": "daemon log unavailable"})
    try:
        check_status = (raw / "check-status.tsv").read_text().strip()
    except OSError:
        check_status = None
    total_delta = delta(total_before, total_after)
    return {
        "chunk_type": env.get("chunk_type"),
        "check_status": check_status,
        "phases": analyze(raw),
        "redis_total_commands_delta": None if total_delta is None else total_delta.get("total"),
        "redis_commands_delta": delta(before, after),
        "s3_final_volume_usage": usage,
        "writeback_errors": writeback_errors,
        "note": "S3 retained volume size is not GET/PUT traffic. Commandstats includes file-system setup and internal script commands; do not equate totals to network round trips.",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm", nargs=2, metavar=("NAME", "OUTPUT_DIR"), action="append", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    report = {name: run_arm(pathlib.Path(directory)) for name, directory in args.arm}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
