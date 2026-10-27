#!/usr/bin/env python3
"""Describe measured generic/346 experimental arms without changing PASS criteria.

Diagnostics are observational only. The strict verify_case gate separately
rejects XUnit failures, missing daemon logs and writeback errors.
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
            counts[match.group(1) if pattern is COMMAND else "total"] = int(match.group(2) if pattern is COMMAND else match.group(1))
    return counts


def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int] | None:
    if not before or not after:
        return None
    return {name: after.get(name, 0) - count for name, count in before.items() if name in after} | {
        name: count for name, count in after.items() if name not in before
    }


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
    command_delta = delta(before, after)
    full_delta = delta(total_before, total_after)
    usage = {}
    for role in ("test", "scratch"):
        try:
            usage[role] = json.loads((raw / f"s3-{role}-volume-usage.json").read_text())
        except (OSError, ValueError):
            usage[role] = None
    writeback_errors = []
    backend_timing = {}
    for role in ("test", "scratch"):
        try:
            for line in (directory / f"swordfs-{role}.log").read_text().splitlines():
                if "FileReadWriter::Flush chunk FAILED" in line or "local RELEASE failed" in line:
                    writeback_errors.append({"role": role, "message": line})
                if "SWORDFS_IO_TIMING " in line:
                    try:
                        backend_timing[role] = json.loads(line.split("SWORDFS_IO_TIMING ", 1)[1])
                    except ValueError:
                        backend_timing[role] = {"error": "invalid timing JSON"}
        except OSError:
            writeback_errors.append({"role": role, "message": "daemon log unavailable"})
    try:
        check_status = (raw / "check-status.tsv").read_text().strip()
    except OSError:
        check_status = None
    # Accumulated operation service time (one worker's observed elapsed time
    # summed across calls) is not wall time: parallel calls may overlap.
    service_time = {"data": 0, "metadata": 0}
    data_traffic_bytes = {"get": 0, "put": 0}
    txn = {name: 0 for name in ("calls", "us", "attempts", "watch_conflicts", "backoff_us")}
    measured = False
    for telemetry in backend_timing.values():
        try:
            data = telemetry["data"]
            meta = telemetry["metadata"]
            transaction = telemetry["transaction"]
            service_time["data"] += data["get"]["us"] + data["put"]["us"]
            service_time["metadata"] += meta["us"]
            for operation in data_traffic_bytes:
                data_traffic_bytes[operation] += data[operation]["bytes"]
            for name in txn:
                txn[name] += transaction[name]
            measured = True
        except (KeyError, TypeError):
            # Any absence is reported below; never synthesize a zero-time
            # result for a failed/crashed daemon or unsupported telemetry.
            continue
    service_time_us = service_time if measured else None
    total_service_us = sum(service_time.values()) if measured else 0
    service_share_pct = (
        {key: round(100 * usecs / total_service_us, 2) for key, usecs in service_time.items()}
        if total_service_us else None
    )
    return {
        "chunk_type": env.get("chunk_type"),
        "strategy": env.get("slice_rmw_strategy"),
        "check_status": check_status,
        "phases": analyze(raw),
        "redis_total_commands_delta": None if full_delta is None else full_delta.get("total"),
        "redis_commands_delta": command_delta,
        "s3_final_volume_usage": usage,
        "writeback_errors": writeback_errors,
        "backend_timing": backend_timing,
        "service_time_us": service_time_us,
        "service_share_pct": service_share_pct,
        "transaction_diagnostics": txn if measured else None,
        "data_traffic_bytes": data_traffic_bytes if measured else None,
        "timed_daemon_count": sum(isinstance(x.get("data"), dict) for x in backend_timing.values()),
        "timing_complete": len(backend_timing) == 2 and all(
            isinstance(x.get("data"), dict) and isinstance(x.get("metadata"), dict)
            and isinstance(x.get("transaction"), dict) for x in backend_timing.values()
        ),
        "note": "S3 volume usage is retained bytes/objects, NOT GET/PUT traffic; no traffic total is claimed.",
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
