#!/usr/bin/env python3
"""Fail closed on a missing/wrong mechanism or nonpassing pinned FUSE case."""
from __future__ import annotations

import argparse
import json
import pathlib
import xml.etree.ElementTree as ET


def verify(output: pathlib.Path, chunk_type: str, test: str) -> None:
    environment = json.loads((output / "environment.json").read_text())
    if environment.get("chunk_type") != chunk_type:
        raise ValueError(f"unexpected chunk type: {environment.get('chunk_type')!r}")
    shard = json.loads((output / "shard.json").read_text())
    if shard.get("planned_tests") != [test]:
        raise ValueError(f"unexpected planned tests: {shard.get('planned_tests')!r}")
    if (output / "raw/check-status.tsv").read_text().strip() != f"{test}\t0":
        raise ValueError(f"actual case did not complete successfully: {test}")
    tests = list(ET.parse(output / "raw/results/result.xml").getroot().iter("testcase"))
    if len(tests) != 1 or tests[0].get("name") != test or list(tests[0]):
        raise ValueError(f"XUnit does not record a single successful case: {test}")
    print(f"{test} PASS on {chunk_type}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    parser.add_argument("--expected-chunk-type", required=True, choices=("cow", "chunk_slice"))
    parser.add_argument("--expected-test", required=True)
    args = parser.parse_args()
    verify(args.output_dir, args.expected_chunk_type, args.expected_test)


if __name__ == "__main__":
    main()
