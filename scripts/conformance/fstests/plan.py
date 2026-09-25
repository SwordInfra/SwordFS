#!/usr/bin/env python3
"""Build and validate the deterministic SwordFS fstests CI shard plan."""

from __future__ import annotations

import argparse
import csv
import io
import json
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

TEST_RE = re.compile(r"^[a-z0-9_-]+/[0-9]+$")
SUPPORTED_SHARD_COUNT = 4
SUPPORTED_SHARDS = tuple(f"supported-{index}" for index in range(SUPPORTED_SHARD_COUNT))
REST_SHARD_COUNT = 2
REST_SHARDS = tuple(f"baseline-rest-{index}" for index in range(REST_SHARD_COUNT))
BASE_SHARD_NAMES = (*SUPPORTED_SHARDS, *REST_SHARDS)
DEFAULT_RUNTIME_SECONDS = 1.0
BOUNDED_SHARD_RE = re.compile(r"^bounded-[a-z0-9_-]+$")


class PlanError(ValueError):
    pass


def validate_test(test: str, source: str) -> None:
    if not TEST_RE.fullmatch(test):
        raise PlanError(f"{source}: invalid exact fstests testcase id: {test!r}")


def load_version(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise PlanError(f"{path}:{line_number}: expected KEY=value")
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def load_selection(path: pathlib.Path) -> tuple[list[str], dict[str, str]]:
    tests: list[str] = []
    seen: set[str] = set()
    metadata: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            match = re.fullmatch(r"#\s*(fstests_commit|fstests_group)=(.+)", line)
            if match:
                metadata[match.group(1)] = match.group(2).strip()
            continue
        validate_test(line, f"{path}:{line_number}")
        if line in seen:
            raise PlanError(f"{path}:{line_number}: duplicate testcase {line}")
        seen.add(line)
        tests.append(line)
    if not tests:
        raise PlanError(f"{path}: selection is empty")
    for key in ("fstests_commit", "fstests_group"):
        if not metadata.get(key):
            raise PlanError(f"{path}: missing # {key}=... metadata")
    return tests, metadata


def validate_selection_identity(metadata: dict[str, str], version: dict[str, str]) -> None:
    expected = {
        "fstests_commit": version.get("FSTESTS_COMMIT", ""),
        "fstests_group": version.get("FSTESTS_GROUP", ""),
    }
    for key, expected_value in expected.items():
        if not expected_value:
            raise PlanError(f"version file is missing {key.upper()}")
        if metadata.get(key) != expected_value:
            raise PlanError(
                f"selection {key}={metadata.get(key)!r} does not match pinned value {expected_value!r}"
            )


def load_supported(path: pathlib.Path) -> set[str]:
    tests: set[str] = set()
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        test = raw.strip()
        if not test or test.startswith("#"):
            continue
        validate_test(test, f"{path}:{line_number}")
        if test in tests:
            raise PlanError(f"{path}:{line_number}: duplicate testcase {test}")
        tests.add(test)
    return tests


def _dict_reader_without_comments(path: pathlib.Path) -> csv.DictReader:
    text = "\n".join(
        line for line in path.read_text(encoding="utf-8").splitlines() if not line.lstrip().startswith("#")
    )
    return csv.DictReader(io.StringIO(text), delimiter="\t")


def load_deferred(path: pathlib.Path) -> set[str]:
    reader = _dict_reader_without_comments(path)
    if reader.fieldnames != ["test", "reason"]:
        raise PlanError(f"{path}: expected TSV header ['test', 'reason'], got {reader.fieldnames}")
    tests: set[str] = set()
    for line_number, row in enumerate(reader, 2):
        test = row["test"].strip()
        if not test:
            continue
        validate_test(test, f"{path}:{line_number}")
        if test in tests:
            raise PlanError(f"{path}:{line_number}: duplicate testcase {test}")
        tests.add(test)
    return tests




def load_bounded(path: pathlib.Path) -> dict[str, str]:
    reader = _dict_reader_without_comments(path)
    if reader.fieldnames != ["test", "shard"]:
        raise PlanError(f"{path}: expected TSV header ['test', 'shard'], got {reader.fieldnames}")
    bounded: dict[str, str] = {}
    shards: set[str] = set()
    for line_number, row in enumerate(reader, 2):
        test = row["test"].strip()
        shard = row["shard"].strip()
        if not test:
            continue
        validate_test(test, f"{path}:{line_number}")
        if test in bounded:
            raise PlanError(f"{path}:{line_number}: duplicate testcase {test}")
        if not BOUNDED_SHARD_RE.fullmatch(shard):
            raise PlanError(f"{path}:{line_number}: invalid bounded shard name {shard!r}")
        if shard in shards:
            raise PlanError(f"{path}:{line_number}: bounded shard {shard!r} must identify exactly one testcase")
        bounded[test] = shard
        shards.add(shard)
    return bounded


def load_runtime(path: pathlib.Path) -> dict[str, float]:
    reader = _dict_reader_without_comments(path)
    if reader.fieldnames != ["test", "seconds"]:
        raise PlanError(f"{path}: expected TSV header ['test', 'seconds'], got {reader.fieldnames}")
    weights: dict[str, float] = {}
    for line_number, row in enumerate(reader, 2):
        test = row["test"].strip()
        if not test:
            continue
        validate_test(test, f"{path}:{line_number}")
        if test in weights:
            raise PlanError(f"{path}:{line_number}: duplicate testcase {test}")
        try:
            seconds = float(row["seconds"])
        except ValueError as exc:
            raise PlanError(f"{path}:{line_number}: invalid runtime for {test}") from exc
        if seconds < 0:
            raise PlanError(f"{path}:{line_number}: negative runtime for {test}")
        weights[test] = seconds
    return weights


def build_plan(
    selected: list[str],
    supported: set[str],
    deferred: set[str],
    runtime: dict[str, float],
    bounded: dict[str, str] | None = None,
) -> dict[str, list[str]]:
    selected_set = set(selected)
    unknown_supported = supported - selected_set
    if unknown_supported:
        raise PlanError("supported testcases are outside the pinned selection: " + ", ".join(sorted(unknown_supported)))
    unknown_deferred = deferred - selected_set
    if unknown_deferred:
        raise PlanError("deferred testcases are outside the pinned selection: " + ", ".join(sorted(unknown_deferred)))
    overlap = supported & deferred
    if overlap:
        raise PlanError("supported and deferred testcases overlap: " + ", ".join(sorted(overlap)))
    bounded = bounded or {}
    bounded_tests = set(bounded)
    unknown_bounded = bounded_tests - selected_set
    if unknown_bounded:
        raise PlanError("bounded testcases are outside the pinned selection: " + ", ".join(sorted(unknown_bounded)))
    deferred_bounded = bounded_tests & deferred
    if deferred_bounded:
        raise PlanError("bounded testcases cannot remain deferred: " + ", ".join(sorted(deferred_bounded)))

    bounded_shards = tuple(sorted(set(bounded.values())))
    shard_names = (*BASE_SHARD_NAMES, *bounded_shards)
    plan: dict[str, list[str]] = {name: [] for name in shard_names}
    for test, shard in bounded.items():
        plan[shard].append(test)
    loads = [0.0] * SUPPORTED_SHARD_COUNT
    supported_selected = sorted(
        supported - bounded_tests,
        key=lambda test: (-max(runtime.get(test, DEFAULT_RUNTIME_SECONDS), DEFAULT_RUNTIME_SECONDS), test),
    )
    for test in supported_selected:
        shard_index = min(
            range(SUPPORTED_SHARD_COUNT),
            key=lambda index: (loads[index], len(plan[SUPPORTED_SHARDS[index]]), index),
        )
        plan[SUPPORTED_SHARDS[shard_index]].append(test)
        loads[shard_index] += max(runtime.get(test, DEFAULT_RUNTIME_SECONDS), DEFAULT_RUNTIME_SECONDS)

    rest_tests = sorted(selected_set - deferred - supported - bounded_tests)
    for index, test in enumerate(rest_tests):
        plan[REST_SHARDS[index % REST_SHARD_COUNT]].append(test)
    for shard in SUPPORTED_SHARDS:
        plan[shard].sort()

    planned = [test for shard in shard_names for test in plan[shard]]
    expected = selected_set - deferred
    if len(planned) != len(set(planned)):
        raise PlanError("generated shard plan contains duplicate testcase IDs")
    if set(planned) != expected:
        missing = sorted(expected - set(planned))
        extra = sorted(set(planned) - expected)
        raise PlanError(f"generated shard plan does not cover selection: missing={missing}, extra={extra}")
    return plan


def verify_selection_xml(path: pathlib.Path, selected: list[str]) -> None:
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        raise PlanError(f"cannot parse upstream selection XUnit {path}: {exc}") from exc
    actual: list[str] = []
    seen: set[str] = set()
    for testcase in root.iter("testcase"):
        test = (testcase.get("name") or "").strip()
        validate_test(test, str(path))
        if test in seen:
            raise PlanError(f"{path}: duplicate testcase {test}")
        seen.add(test)
        actual.append(test)
    expected = set(selected)
    actual_set = set(actual)
    if actual_set != expected:
        missing = sorted(expected - actual_set)
        extra = sorted(actual_set - expected)
        raise PlanError(f"upstream selection differs from pinned manifest: missing={missing}, extra={extra}")


def plan_summary(plan: dict[str, list[str]], runtime: dict[str, float]) -> dict[str, object]:
    shards = {}
    for name in plan:
        tests = plan[name]
        shards[name] = {
            "count": len(tests),
            "runtime_weight_seconds": round(
                sum(max(runtime.get(test, DEFAULT_RUNTIME_SECONDS), DEFAULT_RUNTIME_SECONDS) for test in tests), 3
            ),
        }
    return {"shards": shards, "total_planned": sum(item["count"] for item in shards.values())}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selection", type=pathlib.Path, default=pathlib.Path("conformance/fstests/selected.txt"))
    parser.add_argument("--supported", type=pathlib.Path, default=pathlib.Path("conformance/fstests/supported.txt"))
    parser.add_argument("--deferred", type=pathlib.Path, default=pathlib.Path("conformance/fstests/deferred-ci.tsv"))
    parser.add_argument("--runtime", type=pathlib.Path, default=pathlib.Path("conformance/fstests/runtime.tsv"))
    parser.add_argument("--bounded", type=pathlib.Path, default=pathlib.Path("conformance/fstests/bounded-ci.tsv"))
    parser.add_argument("--version", type=pathlib.Path, default=pathlib.Path("conformance/fstests/version.env"))
    parser.add_argument("--shard")
    parser.add_argument("--list-shards", action="store_true")
    parser.add_argument("--summary-json", action="store_true")
    parser.add_argument("--verify-selection-xml", type=pathlib.Path)
    args = parser.parse_args(argv)

    try:
        selected, metadata = load_selection(args.selection)
        version = load_version(args.version)
        validate_selection_identity(metadata, version)
        supported = load_supported(args.supported)
        deferred = load_deferred(args.deferred)
        runtime = load_runtime(args.runtime)
        bounded = load_bounded(args.bounded)
        plan = build_plan(selected, supported, deferred, runtime, bounded)
        if args.verify_selection_xml:
            verify_selection_xml(args.verify_selection_xml, selected)
    except (OSError, PlanError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.list_shards:
        print("\n".join(plan))
    elif args.shard:
        if args.shard not in plan:
            parser.error(f"unknown shard: {args.shard}")
        print("".join(f"{test}\n" for test in plan[args.shard]), end="")
    elif args.summary_json:
        print(json.dumps(plan_summary(plan, runtime), indent=2, sort_keys=True))
    elif args.verify_selection_xml:
        print(f"verified {len(selected)} selected testcases against upstream dry-run")
    else:
        parser.error("one of --shard, --list-shards, --summary-json, or --verify-selection-xml is required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
