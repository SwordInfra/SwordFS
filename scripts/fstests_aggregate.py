#!/usr/bin/env python3
"""Aggregate fstests shard evidence into one authoritative classifier input."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import shutil
import sys
import xml.etree.ElementTree as ET

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import fstests_plan  # noqa: E402


class AggregateError(ValueError):
    pass


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def load_xunit_cases(path: pathlib.Path) -> list[tuple[str, ET.Element]]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise AggregateError(f"cannot parse {path}: {exc}") from exc
    cases: list[tuple[str, ET.Element]] = []
    seen: set[str] = set()
    for testcase in root.iter():
        if _local_name(testcase.tag) != "testcase":
            continue
        test = (testcase.get("name") or "").strip()
        fstests_plan.validate_test(test, str(path))
        if test in seen:
            raise AggregateError(f"{path}: duplicate testcase {test}")
        seen.add(test)
        cases.append((test, copy.deepcopy(testcase)))
    if not cases:
        raise AggregateError(f"{path}: XUnit contains no testcases")
    return cases


def _read_harness_status(path: pathlib.Path) -> int:
    try:
        value = path.read_text(encoding="utf-8").strip()
        status = int(value)
    except (OSError, ValueError) as exc:
        raise AggregateError(f"cannot read harness status {path}: {exc}") from exc
    if status < 0:
        raise AggregateError(f"{path}: invalid negative harness status {status}")
    return status


def _load_shard_metadata(path: pathlib.Path) -> dict[str, object]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AggregateError(f"cannot parse shard metadata {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise AggregateError(f"{path}: shard metadata must be an object")
    return payload


def _copy_test_evidence(source: pathlib.Path, destination: pathlib.Path, errors: list[str]) -> None:
    if not source.is_dir():
        return
    for group_dir in sorted(path for path in source.iterdir() if path.is_dir()):
        for source_file in sorted(path for path in group_dir.rglob("*") if path.is_file()):
            relative = source_file.relative_to(source)
            target = destination / relative
            if target.exists():
                errors.append(f"duplicate testcase evidence path across shards: {relative}")
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_file, target)


def _aggregate_environment(environments: list[dict[str, object]], shard_count: int) -> dict[str, object]:
    def common_value(key: str) -> object:
        values = {json.dumps(item.get(key), sort_keys=True) for item in environments if key in item}
        if not values:
            return "unknown"
        if len(values) == 1:
            return json.loads(next(iter(values)))
        return "multiple (see shard-environments.json)"

    return {
        "os": common_value("os"),
        "kernel": common_value("kernel"),
        "libfuse": common_value("libfuse"),
        "runner": f"GitHub Actions ({shard_count} fstests shards)",
        "backend": common_value("backend"),
        "fstests_repository": common_value("fstests_repository"),
        "fstests_selector": common_value("fstests_selector"),
        "liburing_commit": common_value("liburing_commit"),
    }


def aggregate(
    shards_dir: pathlib.Path,
    output_dir: pathlib.Path,
    plan: dict[str, list[str]],
    selected: list[str],
    deferred: set[str],
    *,
    artifact_prefix: str = "fstests-conformance-",
    matrix_result: str = "success",
) -> list[str]:
    errors: list[str] = []
    if matrix_result != "success":
        errors.append(f"fstests shard matrix completed with result {matrix_result!r}")
    merged_cases: dict[str, ET.Element] = {}
    environments: list[dict[str, object]] = []
    shard_summaries: dict[str, object] = {}
    result_dir = output_dir / "raw/results"
    result_dir.mkdir(parents=True, exist_ok=True)

    expected_dirs = {f"{artifact_prefix}{shard}" for shard in plan}
    actual_dirs = {path.name for path in shards_dir.iterdir() if path.is_dir()} if shards_dir.is_dir() else set()
    missing_dirs = sorted(expected_dirs - actual_dirs)
    unexpected_dirs = sorted(name for name in actual_dirs - expected_dirs if name.startswith(artifact_prefix))
    if missing_dirs:
        errors.append("missing shard artifacts: " + ", ".join(missing_dirs))
    if unexpected_dirs:
        errors.append("unexpected shard artifacts: " + ", ".join(unexpected_dirs))

    for shard, expected_tests in plan.items():
        artifact = shards_dir / f"{artifact_prefix}{shard}"
        summary: dict[str, object] = {"expected_count": len(expected_tests)}
        shard_summaries[shard] = summary
        if not artifact.is_dir():
            summary["status"] = "MISSING_ARTIFACT"
            continue

        try:
            metadata = _load_shard_metadata(artifact / "shard.json")
            if metadata.get("shard") != shard:
                errors.append(f"{shard}: shard.json identifies {metadata.get('shard')!r}")
            planned_tests = metadata.get("planned_tests")
            if planned_tests != expected_tests:
                errors.append(f"{shard}: planned testcase list differs from deterministic shard plan")
        except AggregateError as exc:
            errors.append(str(exc))

        try:
            harness_status = _read_harness_status(artifact / "harness-status.txt")
            summary["harness_status"] = harness_status
            if harness_status != 0:
                errors.append(f"{shard}: harness exited with status {harness_status}")
        except AggregateError as exc:
            errors.append(str(exc))
            summary["harness_status"] = None

        environment_path = artifact / "environment.json"
        if environment_path.exists():
            try:
                environment = json.loads(environment_path.read_text(encoding="utf-8"))
                if isinstance(environment, dict):
                    environments.append(environment)
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"{shard}: cannot parse environment.json: {exc}")

        result_xml = artifact / "raw/results/result.xml"
        try:
            cases = load_xunit_cases(result_xml)
        except AggregateError as exc:
            errors.append(str(exc))
            summary["actual_count"] = 0
            _copy_test_evidence(artifact / "raw/results", result_dir, errors)
            continue

        actual_tests = [test for test, _ in cases]
        actual_set = set(actual_tests)
        expected_set = set(expected_tests)
        summary["actual_count"] = len(actual_tests)
        if actual_set != expected_set:
            missing = sorted(expected_set - actual_set)
            extra = sorted(actual_set - expected_set)
            errors.append(f"{shard}: XUnit population mismatch: missing={missing}, extra={extra}")
        for test, testcase in cases:
            if test in merged_cases:
                errors.append(f"duplicate testcase result across shards: {test}")
                continue
            merged_cases[test] = testcase
        _copy_test_evidence(artifact / "raw/results", result_dir, errors)
        summary["status"] = "COMPLETE" if actual_set == expected_set else "INCOMPLETE"

    selected_set = set(selected)
    expected_population = selected_set - deferred
    actual_population = set(merged_cases)
    executed_deferred = sorted(actual_population & deferred)
    if executed_deferred:
        errors.append("deferred testcase appeared in shard results: " + ", ".join(executed_deferred))
    missing_population = sorted(expected_population - actual_population)
    extra_population = sorted(actual_population - expected_population)
    if missing_population:
        errors.append("aggregate result is missing selected testcase(s): " + ", ".join(missing_population))
    if extra_population:
        errors.append("aggregate result contains unexpected testcase(s): " + ", ".join(extra_population))

    suite = ET.Element("testsuite", name="xfstests-aggregate", tests=str(len(merged_cases)))
    for test in sorted(merged_cases):
        suite.append(merged_cases[test])
    root = ET.Element("testsuites")
    root.append(suite)
    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    tree.write(result_dir / "result.xml", encoding="utf-8", xml_declaration=True)

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "harness-status.txt").write_text("0\n" if not errors else "2\n", encoding="utf-8")
    (output_dir / "aggregate-errors.txt").write_text("".join(f"{error}\n" for error in errors), encoding="utf-8")
    (output_dir / "shards.json").write_text(json.dumps(shard_summaries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_dir / "shard-environments.json").write_text(
        json.dumps(environments, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (output_dir / "environment.json").write_text(
        json.dumps(_aggregate_environment(environments, len(plan)), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shards-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--artifact-prefix", default="fstests-conformance-")
    parser.add_argument("--matrix-result", default="success")
    parser.add_argument("--selection", type=pathlib.Path, default=pathlib.Path("conformance/fstests/selected.txt"))
    parser.add_argument("--supported", type=pathlib.Path, default=pathlib.Path("conformance/fstests/supported.txt"))
    parser.add_argument("--deferred", type=pathlib.Path, default=pathlib.Path("conformance/fstests/deferred-ci.tsv"))
    parser.add_argument("--runtime", type=pathlib.Path, default=pathlib.Path("conformance/fstests/runtime.tsv"))
    parser.add_argument("--version", type=pathlib.Path, default=pathlib.Path("conformance/fstests/version.env"))
    args = parser.parse_args(argv)

    try:
        selected, metadata = fstests_plan.load_selection(args.selection)
        version = fstests_plan.load_version(args.version)
        fstests_plan.validate_selection_identity(metadata, version)
        supported = fstests_plan.load_supported(args.supported)
        deferred = fstests_plan.load_deferred(args.deferred)
        runtime = fstests_plan.load_runtime(args.runtime)
        plan = fstests_plan.build_plan(selected, supported, deferred, runtime)
        errors = aggregate(
            args.shards_dir,
            args.output_dir,
            plan,
            selected,
            deferred,
            artifact_prefix=args.artifact_prefix,
            matrix_result=args.matrix_result,
        )
    except (OSError, fstests_plan.PlanError, AggregateError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if errors:
        print("fstests aggregate validation found infrastructure errors:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
    else:
        print(f"aggregated {sum(len(tests) for tests in plan.values())} testcase results across {len(plan)} shards")
    # Population/shard errors are intentionally encoded in harness-status.txt so
    # the classifier can produce the authoritative blocking report.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
