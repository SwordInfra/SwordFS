#!/usr/bin/env python3
"""Classify testcase-level fstests XUnit results against the SwordFS baseline."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import pathlib
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from typing import Iterable


GAP_CATEGORIES = {
    "known_semantic_defect": "KNOWN_SEMANTIC_DEFECT",
    "known_unsupported": "KNOWN_UNSUPPORTED",
    "upstream_not_applicable": "UPSTREAM_NOT_APPLICABLE",
    "environment": "ENVIRONMENT",
}
BLOCKING = {
    "REGRESSION",
    "XPASS",
    "UNCLASSIFIED_PASS",
    "UNCLASSIFIED_NOTRUN",
    "UNEXPECTED_FAIL",
    "BASELINE_NOT_SELECTED",
    "BASELINE_NOT_APPLICABLE",
    "BASELINE_RESULT_MISMATCH",
    "BASELINE_REASON_MISMATCH",
    "INFRASTRUCTURE",
    "SWORD_FS_CRASH",
}
ISSUE_RE = re.compile(r"^#[1-9][0-9]*$")
TEST_RE = re.compile(r"^[a-z0-9_-]+/[0-9]+$")
WORK_DIR_RE = re.compile(r"/tmp/swordfs-fstests\.[^/\s)]+")
RESULT_DIR_RE = re.compile(r"/(?:[^/\s)]+/)*build/fstests-conformance/raw/results")
SWORDFS_CORE_RE = re.compile(r"^\s*Message:\s+Process\s+\d+\s+\(swordfs\).*dumped core\.\s*$", re.MULTILINE)


class BaselineError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class RawResult:
    test: str
    result: str
    message: str = ""


@dataclasses.dataclass(frozen=True)
class Gap:
    category: str
    issue: str
    expected_result: str
    expected_message: str
    reason: str


def load_deferred(path: pathlib.Path) -> dict[str, str]:
    deferred: dict[str, str] = {}
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        expected_fields = ["test", "reason"]
        if reader.fieldnames != expected_fields:
            raise BaselineError(f"{path}: expected TSV header {expected_fields}, got {reader.fieldnames}")
        for line_number, row in enumerate(reader, start=2):
            test = row["test"].strip()
            if not test or test.startswith("#"):
                continue
            _validate_test_id(test, f"{path}:{line_number}")
            if test in deferred:
                raise BaselineError(f"{path}:{line_number}: duplicate testcase {test}")
            reason = normalize_message(row["reason"])
            if not reason:
                raise BaselineError(f"{path}:{line_number}: reason is required")
            deferred[test] = reason
    return deferred


@dataclasses.dataclass
class Observation:
    test: str
    result: str
    classification: str
    message: str = ""
    issue: str = ""
    reason: str = ""

    def as_dict(self) -> dict[str, str]:
        return dataclasses.asdict(self)


def normalize_message(value: str | None) -> str:
    normalized = " ".join((value or "").split())
    # The privileged runner creates a fresh mktemp work directory for every
    # execution. Upstream NOTRUN reasons can embed TEST_DIR/SCRATCH_MNT, which
    # would otherwise make an unchanged semantic result fail the next run only
    # because the random mktemp suffix changed. Canonicalize exactly the
    # harness-owned root while preserving the meaningful relative path/reason.
    normalized = WORK_DIR_RE.sub("<FSTESTS_WORK_DIR>", normalized)
    # Upstream XUnit FAIL messages include the absolute path of the .out.bad
    # or .mountfail artifact. The testcase-specific suffix is useful evidence,
    # but the GitHub workspace prefix is runner noise.
    return RESULT_DIR_RE.sub("<FSTESTS_RESULT_DIR>", normalized)


def _validate_test_id(test: str, source: str) -> None:
    if not TEST_RE.fullmatch(test):
        raise BaselineError(f"{source}: invalid exact fstests testcase id: {test!r}")


def load_supported(path: pathlib.Path) -> set[str]:
    supported: set[str] = set()
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        _validate_test_id(line, f"{path}:{line_number}")
        if line in supported:
            raise BaselineError(f"{path}:{line_number}: duplicate testcase {line}")
        supported.add(line)
    return supported


def load_gaps(path: pathlib.Path) -> dict[str, Gap]:
    gaps: dict[str, Gap] = {}
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        expected_fields = ["test", "category", "issue", "expected_result", "expected_message", "reason"]
        if reader.fieldnames != expected_fields:
            raise BaselineError(f"{path}: expected TSV header {expected_fields}, got {reader.fieldnames}")
        for line_number, row in enumerate(reader, start=2):
            test = row["test"].strip()
            if not test or test.startswith("#"):
                continue
            _validate_test_id(test, f"{path}:{line_number}")
            if test in gaps:
                raise BaselineError(f"{path}:{line_number}: duplicate testcase {test}")

            category = row["category"].strip().lower()
            if category not in GAP_CATEGORIES:
                raise BaselineError(f"{path}:{line_number}: invalid category {category!r}")
            issue = row["issue"].strip()
            expected_result = row["expected_result"].strip().upper()
            expected_message = normalize_message(row["expected_message"])
            reason = normalize_message(row["reason"])
            if not reason:
                raise BaselineError(f"{path}:{line_number}: reason is required")

            if category in {"known_semantic_defect", "known_unsupported"}:
                if not ISSUE_RE.fullmatch(issue):
                    raise BaselineError(f"{path}:{line_number}: {category} requires a #<issue> reference")
            else:
                if issue not in {"", "-"}:
                    raise BaselineError(f"{path}:{line_number}: {category} must not use a semantic Issue")

            if expected_result not in {"FAIL", "NOTRUN"}:
                raise BaselineError(f"{path}:{line_number}: {category} must expect FAIL or NOTRUN")

            if expected_result == "NOTRUN" and not expected_message:
                raise BaselineError(f"{path}:{line_number}: NOTRUN entries must preserve the upstream skip reason")
            if expected_result == "FAIL" and expected_message in {"", "-"}:
                raise BaselineError(
                    f"{path}:{line_number}: {category} FAIL entries must preserve the exact failure message"
                )

            gaps[test] = Gap(category, issue, expected_result, expected_message, reason)
    return gaps


def validate_supported_monotonicity(current: set[str], previous: set[str], allow_removal: bool) -> None:
    removed = sorted(previous - current)
    if removed and not allow_removal:
        raise BaselineError(
            "supported fstests coverage cannot shrink without an explicit semantic-contract override: "
            + ", ".join(removed)
        )


def validate_deferred_growth(current: dict[str, str], previous: dict[str, str], allow_addition: bool) -> None:
    added = sorted(current.keys() - previous.keys())
    if added and not allow_addition:
        raise BaselineError(
            "deferred fstests coverage cannot grow without an explicit reviewed override: " + ", ".join(added)
        )


def load_xunit(path: pathlib.Path) -> dict[str, RawResult]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise BaselineError(f"cannot parse fstests XUnit {path}: {exc}") from exc

    results: dict[str, RawResult] = {}
    for testcase in root.findall(".//testcase"):
        test = (testcase.get("name") or "").strip()
        _validate_test_id(test, str(path))
        if test in results:
            raise BaselineError(f"{path}: duplicate testcase {test}")
        failure = testcase.find("failure")
        skipped = testcase.find("skipped")
        if failure is not None:
            result = "FAIL"
            message = normalize_message(failure.get("message") or failure.text)
        elif skipped is not None:
            result = "NOTRUN"
            message = normalize_message(skipped.get("message") or skipped.text)
        else:
            result = "PASS"
            message = ""
        results[test] = RawResult(test, result, message)
    if not results:
        raise BaselineError(f"{path}: XUnit contains no testcases")
    return results


def try_load_xunit(path: pathlib.Path, label: str) -> tuple[dict[str, RawResult], str | None]:
    try:
        return load_xunit(path), None
    except BaselineError as exc:
        return {}, f"{label}: {exc}"


def load_isolation_failure_tests(result_dir: pathlib.Path) -> set[str]:
    """Return testcase IDs for which the SwordFS runner had to restore isolation."""
    isolation_failure_tests: set[str] = set()
    if not result_dir.exists():
        # A harness/preflight failure can occur before fstests creates its
        # result directory. Let the missing XUnit + harness-status evidence be
        # reported through the normal INFRASTRUCTURE path instead of aborting
        # before report.md/result.json can be produced.
        return isolation_failure_tests
    if not result_dir.is_dir():
        raise BaselineError(f"fstests result directory does not exist: {result_dir}")
    for path in result_dir.glob("*/*.isolationfail"):
        relative = path.relative_to(result_dir)
        if len(relative.parts) != 2:
            raise BaselineError(f"unexpected fstests isolation-failure path: {path}")
        test = f"{relative.parts[0]}/{path.stem}"
        _validate_test_id(test, str(path))
        isolation_failure_tests.add(test)
    return isolation_failure_tests


def load_swordfs_crash_tests(result_dir: pathlib.Path) -> set[str]:
    """Return testcase IDs whose .full evidence records a SwordFS daemon core dump."""
    crash_tests: set[str] = set()
    if not result_dir.exists():
        return crash_tests
    if not result_dir.is_dir():
        raise BaselineError(f"fstests result directory does not exist: {result_dir}")
    for path in result_dir.glob("*/*.full"):
        text = path.read_text(encoding="utf-8", errors="replace")
        if not SWORDFS_CORE_RE.search(text):
            continue
        relative = path.relative_to(result_dir)
        if len(relative.parts) != 2:
            raise BaselineError(f"unexpected fstests full-result path: {path}")
        test = f"{relative.parts[0]}/{path.stem}"
        _validate_test_id(test, str(path))
        crash_tests.add(test)
    return crash_tests


def _infrastructure(test: str, message: str) -> Observation:
    return Observation(test=test, result="INFRASTRUCTURE", classification="INFRASTRUCTURE", message=message)


def classify(
    selected: set[str],
    actual: dict[str, RawResult],
    supported: set[str],
    gaps: dict[str, Gap],
    deferred: dict[str, str] | None = None,
    *,
    isolation_failure_tests: set[str] | None = None,
    crash_tests: set[str] | None = None,
    infrastructure: Iterable[str] = (),
) -> list[Observation]:
    deferred = deferred or {}
    isolation_failure_tests = isolation_failure_tests or set()
    crash_tests = crash_tests or set()
    overlap = supported & gaps.keys()
    if overlap:
        raise BaselineError("supported.txt and known-gaps.tsv overlap: " + ", ".join(sorted(overlap)))
    deferred_overlap = deferred.keys() & (supported | gaps.keys())
    if deferred_overlap:
        raise BaselineError(
            "deferred-ci.tsv must not claim supported/known-gap testcases: " + ", ".join(sorted(deferred_overlap))
        )

    observations = [_infrastructure(f"infrastructure/{index}", message) for index, message in enumerate(infrastructure, 1)]

    for test in sorted((supported | gaps.keys() | deferred.keys()) - selected):
        observations.append(
            Observation(test, "MISSING", "BASELINE_NOT_SELECTED", reason="baseline testcase is outside the pinned selector")
        )
    for test in sorted(set(actual) - selected):
        observations.append(_infrastructure(test, "actual XUnit contains a testcase absent from the dry-run selection"))
    for test in sorted(selected - set(actual)):
        if test in crash_tests:
            observations.append(
                Observation(test, "CRASH", "SWORD_FS_CRASH", "SwordFS daemon dumped core during testcase (.full evidence)")
            )
        elif test in deferred:
            observations.append(Observation(test, "DEFERRED", "DEFERRED_CI", reason=deferred[test]))
        else:
            observations.append(_infrastructure(test, "selected testcase has no result; the fstests run is incomplete"))

    for test in sorted(selected & actual.keys()):
        raw = actual[test]
        if test in crash_tests:
            observations.append(
                Observation(
                    test,
                    raw.result,
                    "SWORD_FS_CRASH",
                    "SwordFS daemon dumped core during testcase (.full evidence)",
                )
            )
            continue
        if test in isolation_failure_tests:
            observations.append(
                Observation(
                    test,
                    raw.result,
                    "INFRASTRUCTURE",
                    "SwordFS runner had to intervene to restore testcase isolation (.isolationfail)",
                )
            )
            continue
        gap = gaps.get(test)
        if raw.result == "PASS":
            if test in supported:
                classification = "PASS"
            elif gap is not None:
                classification = "XPASS"
            else:
                classification = "UNCLASSIFIED_PASS"
            observations.append(
                Observation(test, raw.result, classification, raw.message, gap.issue if gap else "", gap.reason if gap else "")
            )
            continue

        if raw.result == "FAIL":
            if test in supported:
                classification = "REGRESSION"
            elif gap is None:
                classification = "UNEXPECTED_FAIL"
            elif gap.expected_result != "FAIL":
                classification = "BASELINE_RESULT_MISMATCH"
            elif gap.expected_message not in {"", "-"} and normalize_message(raw.message) != gap.expected_message:
                classification = "BASELINE_REASON_MISMATCH"
            else:
                classification = GAP_CATEGORIES[gap.category]
            observations.append(
                Observation(test, raw.result, classification, raw.message, gap.issue if gap else "", gap.reason if gap else "")
            )
            continue

        if test in supported:
            classification = "BASELINE_NOT_APPLICABLE"
        elif gap is None:
            classification = "UNCLASSIFIED_NOTRUN"
        elif gap.expected_result != "NOTRUN":
            classification = "BASELINE_RESULT_MISMATCH"
        elif normalize_message(raw.message) != gap.expected_message:
            classification = "BASELINE_REASON_MISMATCH"
        else:
            classification = GAP_CATEGORIES[gap.category]
        observations.append(
            Observation(test, raw.result, classification, raw.message, gap.issue if gap else "", gap.reason if gap else "")
        )
    return observations


def summarize(
    observations: list[Observation],
    selected: set[str],
    supported: set[str],
    gaps: dict[str, Gap],
    deferred: dict[str, str],
) -> dict[str, object]:
    counts = Counter(observation.classification for observation in observations)
    selected_supported = selected & supported
    supported_pass = sum(1 for observation in observations if observation.test in selected_supported and observation.classification == "PASS")
    classified = len(selected & (supported | gaps.keys()))
    selected_deferred = selected & deferred.keys()
    observed_selected = {
        observation.test
        for observation in observations
        if observation.test in selected and observation.result in {"PASS", "FAIL", "NOTRUN"}
    }
    executed_count = len(observed_selected)
    executed_classified = len(observed_selected & (supported | gaps.keys()))
    return {
        "selected_count": len(selected),
        "executed_count": executed_count,
        "deferred_count": len(selected_deferred),
        "supported_count": len(selected_supported),
        "known_gap_count": len(selected & gaps.keys()),
        "supported_pass_count": supported_pass,
        "supported_gate_percent": round(100.0 * supported_pass / len(selected_supported), 2) if selected_supported else 0.0,
        "classified_count": classified,
        "classified_percent": round(100.0 * classified / len(selected), 2) if selected else 0.0,
        "executed_classified_percent": round(100.0 * executed_classified / executed_count, 2) if executed_count else 0.0,
        "blocking_count": sum(counts[name] for name in BLOCKING),
        "classifications": dict(sorted(counts.items())),
    }


def _percent(value: object) -> str:
    return f"{float(value):.2f}%"


def render_markdown(payload: dict[str, object], observations: list[Observation]) -> str:
    metadata = payload["metadata"]
    summary = payload["summary"]
    assert isinstance(metadata, dict) and isinstance(summary, dict)
    status = "PASS" if summary["blocking_count"] == 0 else "BLOCKED"
    lines = [
        "# SwordFS fstests conformance",
        "",
        f"Result: **{status}**",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Selected upstream testcases | {summary['selected_count']} |",
        f"| Executed in PR CI | {summary['executed_count']} |",
        f"| Deferred from PR CI | {summary['deferred_count']} |",
        f"| Explicitly supported | {summary['supported_count']} |",
        f"| Known gaps | {summary['known_gap_count']} |",
        f"| Supported gate | {_percent(summary['supported_gate_percent'])} |",
        f"| Executed population classified | {_percent(summary['executed_classified_percent'])} |",
        f"| Full selected population classified | {_percent(summary['classified_percent'])} |",
        f"| Blocking outcomes | {summary['blocking_count']} |",
        "",
        "## Outcome counts",
        "",
        "| Classification | Count |",
        "| --- | ---: |",
    ]
    classifications = summary.get("classifications", {})
    assert isinstance(classifications, dict)
    for name, count in classifications.items():
        lines.append(f"| `{name}` | {count} |")

    deferred = [observation for observation in observations if observation.classification == "DEFERRED_CI"]
    if deferred:
        lines.extend(["", "## Deferred from PR CI", "", "| Test | Reason |", "| --- | --- |"])
        for observation in deferred:
            lines.append(f"| `{observation.test}` | {observation.reason.replace('|', '\\|')} |")

    blockers = [observation for observation in observations if observation.classification in BLOCKING]
    if blockers:
        lines.extend(["", "## Blocking results", "", "| Test | Classification | Detail |", "| --- | --- | --- |"])
        for observation in blockers[:100]:
            detail = observation.message or observation.reason or "-"
            detail = detail.replace("|", "\\|")
            lines.append(f"| `{observation.test}` | `{observation.classification}` | {detail} |")
        if len(blockers) > 100:
            lines.append(f"\nOnly the first 100 of {len(blockers)} blockers are shown; see `result.json` for all results.")

    issue_groups: dict[str, list[Observation]] = defaultdict(list)
    for observation in observations:
        if ISSUE_RE.fullmatch(observation.issue):
            issue_groups[observation.issue].append(observation)
    if issue_groups:
        lines.extend(["", "## Known-gap Issues", ""])
        for issue, items in sorted(issue_groups.items(), key=lambda item: int(item[0][1:])):
            issue_number = issue[1:]
            categories = ", ".join(sorted({item.classification for item in items}))
            lines.append(
                f"- [{issue}](https://github.com/SwordInfra/SwordFS/issues/{issue_number}): "
                f"{len(items)} testcase(s), {categories}"
            )

    lines.extend(
        [
            "",
            "## Execution metadata",
            "",
            f"- SwordFS commit: `{metadata.get('swordfs_commit', 'unknown')}`",
            f"- fstests commit: `{metadata.get('fstests_commit', 'unknown')}`",
            f"- fstests selector: `{metadata.get('fstests_group', 'unknown')}`",
            f"- Kernel: `{metadata.get('kernel', 'unknown')}`",
            f"- libfuse: `{metadata.get('libfuse', 'unknown')}`",
            f"- Runner: `{metadata.get('runner', 'unknown')}`",
            "",
        ]
    )
    return "\n".join(lines)


def write_bootstrap(output_dir: pathlib.Path, observations: list[Observation]) -> None:
    passes = sorted(observation.test for observation in observations if observation.classification == "UNCLASSIFIED_PASS")
    (output_dir / "bootstrap-supported.txt").write_text(
        "".join(f"{test}\n" for test in passes), encoding="utf-8"
    )

    rows = ["test\tcategory\tissue\texpected_result\texpected_message\treason\n"]
    for observation in observations:
        if observation.classification not in {"UNEXPECTED_FAIL", "UNCLASSIFIED_NOTRUN"}:
            continue
        # Bootstrap output is investigation input, not an auto-approved
        # baseline. Preserve the observed message for both NOTRUN and FAIL so
        # a reviewer can bind a known gap to the exact failure mode instead of
        # accidentally accepting any future failure from the same testcase.
        expected_message = observation.message or "-"
        rows.append(
            f"{observation.test}\tTODO\t#TODO\t{observation.result}\t{expected_message}\tclassify root cause\n"
        )
    (output_dir / "bootstrap-gaps.tsv").write_text("".join(rows), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selected-xml", type=pathlib.Path, required=True)
    parser.add_argument("--result-xml", type=pathlib.Path, required=True)
    parser.add_argument("--result-dir", type=pathlib.Path, required=True)
    parser.add_argument("--supported-file", type=pathlib.Path, required=True)
    parser.add_argument("--known-gaps-file", type=pathlib.Path, required=True)
    parser.add_argument("--deferred-file", type=pathlib.Path, required=True)
    parser.add_argument("--previous-supported-file", type=pathlib.Path)
    parser.add_argument("--allow-supported-removal", action="store_true")
    parser.add_argument("--previous-deferred-file", type=pathlib.Path)
    parser.add_argument("--allow-deferred-addition", action="store_true")
    parser.add_argument("--environment-json", type=pathlib.Path)
    parser.add_argument("--harness-status-file", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--swordfs-commit", required=True)
    parser.add_argument("--fstests-commit", required=True)
    parser.add_argument("--fstests-group", required=True)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args(argv)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    infrastructure: list[str] = []
    try:
        supported = load_supported(args.supported_file)
        gaps = load_gaps(args.known_gaps_file)
        deferred = load_deferred(args.deferred_file)
        isolation_failure_tests = load_isolation_failure_tests(args.result_dir)
        crash_tests = load_swordfs_crash_tests(args.result_dir)
        if args.previous_supported_file:
            previous = load_supported(args.previous_supported_file)
            validate_supported_monotonicity(supported, previous, args.allow_supported_removal)
        if args.previous_deferred_file:
            previous_deferred = load_deferred(args.previous_deferred_file)
            validate_deferred_growth(deferred, previous_deferred, args.allow_deferred_addition)
    except BaselineError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


    selected_results, selected_error = try_load_xunit(args.selected_xml, "selection XUnit")
    actual, actual_error = try_load_xunit(args.result_xml, "result XUnit")
    selected = set(selected_results)
    if selected_error:
        infrastructure.append(selected_error)
    if actual_error:
        infrastructure.append(actual_error)

    if args.harness_status_file and args.harness_status_file.exists():
        status = args.harness_status_file.read_text(encoding="utf-8").strip()
        if status not in {"", "0"}:
            infrastructure.append(f"fstests harness exited with status {status}")

    observations = classify(
        selected,
        actual,
        supported,
        gaps,
        deferred,
        isolation_failure_tests=isolation_failure_tests,
        crash_tests=crash_tests,
        infrastructure=infrastructure,
    )
    summary = summarize(observations, selected, supported, gaps, deferred)

    metadata: dict[str, object] = {}
    if args.environment_json and args.environment_json.exists():
        metadata.update(json.loads(args.environment_json.read_text(encoding="utf-8")))
    metadata.update(
        {
            "swordfs_commit": args.swordfs_commit,
            "fstests_commit": args.fstests_commit,
            "fstests_group": args.fstests_group,
        }
    )
    payload = {"metadata": metadata, "summary": summary, "tests": [item.as_dict() for item in observations]}
    (args.output_dir / "result.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    report = render_markdown(payload, observations)
    (args.output_dir / "report.md").write_text(report, encoding="utf-8")
    write_bootstrap(args.output_dir, observations)
    print(report)

    if args.strict and summary["blocking_count"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
