#!/usr/bin/env python3
"""Fail closed on a missing/wrong mechanism or nonpassing pinned FUSE case."""
from __future__ import annotations

import argparse
import json
import pathlib
import xml.etree.ElementTree as ET


def verify(output: pathlib.Path, chunk_type: str, test: str, strategy: str | None = None) -> None:
    environment = json.loads((output / "environment.json").read_text())
    if environment.get("chunk_type") != chunk_type:
        raise ValueError(f"unexpected chunk type: {environment.get('chunk_type')!r}")
    if strategy is not None and environment.get("slice_rmw_strategy") != strategy:
        raise ValueError(f"unexpected Slice RMW strategy: {environment.get('slice_rmw_strategy')!r}")
    shard = json.loads((output / "shard.json").read_text())
    if shard.get("planned_tests") != [test]:
        raise ValueError(f"unexpected planned tests: {shard.get('planned_tests')!r}")
    if (output / "raw/check-status.tsv").read_text().strip() != f"{test}\t0":
        raise ValueError(f"actual case did not complete successfully: {test}")
    tests = list(ET.parse(output / "raw/results/result.xml").getroot().iter("testcase"))
    if len(tests) != 1 or tests[0].get("name") != test or list(tests[0]):
        raise ValueError(f"XUnit does not record a single successful case: {test}")
    # A FUSE RELEASE writeback error cannot retroactively change a prior
    # successful userspace pwrite/mmap or the upstream xfstests exit status.
    # Treat recorded writeback failures as a correctness regression even when
    # the pinned upstream checker reports PASS. Both daemon roles are audited;
    # missing logs fail closed instead of silently passing the check.
    writeback_errors = ("FileReadWriter::Flush chunk FAILED", "local RELEASE failed")
    for role in ("test", "scratch"):
        for line in (output / f"swordfs-{role}.log").read_text().splitlines():
            if any(marker in line for marker in writeback_errors):
                raise ValueError(f"{role} FUSE daemon reported writeback error: {line}")
    print(f"{test} PASS with clean writeback logs on {chunk_type}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    parser.add_argument("--expected-chunk-type", required=True, choices=("cow", "chunk_slice"))
    parser.add_argument("--expected-test", required=True)
    parser.add_argument("--expected-strategy", choices=("sparse", "fixed-1m", "span-1m"))
    args = parser.parse_args()
    verify(args.output_dir, args.expected_chunk_type, args.expected_test, args.expected_strategy)


if __name__ == "__main__":
    main()
