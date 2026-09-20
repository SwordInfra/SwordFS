#!/usr/bin/env python3
"""Classify assertion-level pjdfstest TAP against the SwordFS baseline."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import pathlib
import re
import sys
from typing import Iterable


ASSERTION_RE = re.compile(
    r"^(not )?ok(?:\s+(\d+))?(?:\s*-\s*(.*?))?(?:\s+#\s*(TODO|SKIP)\b\s*(.*))?$",
    re.IGNORECASE,
)
PLAN_RE = re.compile(r"^1\.\.(\d+)(?:\s+#\s*SKIP\b\s*(.*))?$", re.IGNORECASE)

GAP_CATEGORIES = {
    "known_unsupported": "KNOWN_UNSUPPORTED",
    "known_semantic_defect": "KNOWN_SEMANTIC_DEFECT",
    "environment": "ENVIRONMENT",
}

BLOCKING_CLASSIFICATIONS = {
    "REGRESSION",
    "XPASS",
    "UNEXPECTED_FAIL",
    "UNCLASSIFIED_PASS",
    "INFRASTRUCTURE",
    "BASELINE_NOT_OBSERVED",
    "BASELINE_NOT_APPLICABLE",
}


@dataclasses.dataclass(frozen=True)
class Gap:
    category: str
    issue: str
    reason: str


@dataclasses.dataclass
class Observation:
    assertion_id: str
    test_path: str
    category: str
    number: int | None
    result: str
    message: str = ""
    directive: str = ""
    directive_reason: str = ""
    classification: str = ""
    issue: str = ""
    reason: str = ""

    def as_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


class BaselineError(ValueError):
    pass


def _read_non_comment_lines(path: pathlib.Path) -> Iterable[tuple[int, str]]:
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if line and not line.startswith("#"):
            yield lineno, line


def expand_assertion_selector(selector: str) -> list[str]:
    if "#" not in selector:
        raise BaselineError(f"invalid assertion selector {selector!r}: missing #")
    test_path, number_spec = selector.rsplit("#", 1)
    if not test_path or not number_spec:
        raise BaselineError(f"invalid assertion selector {selector!r}")
    if not test_path.startswith("tests/") or not test_path.endswith(".t"):
        raise BaselineError(f"invalid assertion selector {selector!r}: expected tests/<path>.t#...")
    if any(character in test_path for character in "*?["):
        raise BaselineError(f"invalid assertion selector {selector!r}: wildcards are not supported")

    numbers: list[int] = []
    for part in number_spec.split(","):
        part = part.strip()
        if not part:
            raise BaselineError(f"invalid assertion selector {selector!r}: empty range element")
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            if not start_text.isdigit() or not end_text.isdigit():
                raise BaselineError(f"invalid assertion range {part!r} in {selector!r}")
            start = int(start_text)
            end = int(end_text)
            if start < 1 or end < start:
                raise BaselineError(f"invalid assertion range {part!r} in {selector!r}")
            numbers.extend(range(start, end + 1))
        else:
            if not part.isdigit() or int(part) < 1:
                raise BaselineError(f"invalid assertion number {part!r} in {selector!r}")
            numbers.append(int(part))

    if len(numbers) != len(set(numbers)):
        raise BaselineError(f"selector contains duplicate assertions: {selector!r}")
    return [f"{test_path}#{number}" for number in numbers]


def compact_assertion_ids(assertion_ids: Iterable[str]) -> list[str]:
    by_path: dict[str, list[int]] = collections.defaultdict(list)
    for assertion_id in assertion_ids:
        if "#" not in assertion_id:
            raise BaselineError(f"cannot compact invalid assertion ID {assertion_id!r}")
        test_path, number_text = assertion_id.rsplit("#", 1)
        if not number_text.isdigit():
            raise BaselineError(f"cannot compact non-numeric assertion ID {assertion_id!r}")
        by_path[test_path].append(int(number_text))

    selectors: list[str] = []
    for test_path in sorted(by_path):
        numbers = sorted(set(by_path[test_path]))
        ranges: list[str] = []
        start = previous = numbers[0]
        for number in numbers[1:]:
            if number == previous + 1:
                previous = number
                continue
            ranges.append(str(start) if start == previous else f"{start}-{previous}")
            start = previous = number
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        selectors.append(f"{test_path}#{','.join(ranges)}")
    return selectors


def load_supported(path: pathlib.Path) -> set[str]:
    supported: set[str] = set()
    for lineno, line in _read_non_comment_lines(path):
        try:
            assertions = expand_assertion_selector(line)
        except BaselineError as error:
            raise BaselineError(f"{path}:{lineno}: {error}") from error
        duplicate = supported.intersection(assertions)
        if duplicate:
            raise BaselineError(f"{path}:{lineno}: duplicate supported assertion {sorted(duplicate)[0]}")
        supported.update(assertions)
    return supported


def validate_supported_monotonicity(
    supported: set[str], previous_supported: set[str], allow_removal: bool
) -> None:
    removed_supported = previous_supported - supported
    if removed_supported and not allow_removal:
        preview = ", ".join(sorted(removed_supported)[:10])
        if len(removed_supported) > 10:
            preview += ", ..."
        raise BaselineError(
            "supported assertions are monotonic; removed entries require an explicit "
            f"semantic-contract change: {preview}"
        )


def load_gaps(path: pathlib.Path) -> dict[str, Gap]:
    gaps: dict[str, Gap] = {}
    lines = list(_read_non_comment_lines(path))
    if not lines:
        return gaps

    first_lineno, first = lines[0]
    if first.split("\t") != ["selector", "category", "issue", "reason"]:
        raise BaselineError(f"{path}:{first_lineno}: expected TSV header: selector, category, issue, reason")

    for lineno, line in lines[1:]:
        fields = line.split("\t", 3)
        if len(fields) != 4:
            raise BaselineError(f"{path}:{lineno}: expected four tab-separated fields")
        selector, category, issue, reason = (field.strip() for field in fields)
        if not selector:
            raise BaselineError(f"{path}:{lineno}: missing assertion selector")
        if category not in GAP_CATEGORIES:
            raise BaselineError(
                f"{path}:{lineno}: unknown category {category!r}; expected one of {sorted(GAP_CATEGORIES)}"
            )
        if category != "environment" and (not issue or issue == "-"):
            raise BaselineError(f"{path}:{lineno}: semantic gaps must link a focused GitHub Issue")
        if category != "environment" and not re.fullmatch(r"#\d+", issue):
            raise BaselineError(f"{path}:{lineno}: issue must use the form #<number>, got {issue!r}")
        if not reason:
            raise BaselineError(f"{path}:{lineno}: gap reason must not be empty")
        try:
            assertion_ids = expand_assertion_selector(selector)
        except BaselineError as error:
            raise BaselineError(f"{path}:{lineno}: {error}") from error
        duplicate = gaps.keys() & set(assertion_ids)
        if duplicate:
            raise BaselineError(f"{path}:{lineno}: duplicate gap assertion {sorted(duplicate)[0]}")
        for assertion_id in assertion_ids:
            gaps[assertion_id] = Gap(category=category, issue=issue, reason=reason)
    return gaps


def _category_for_test(test_path: str) -> str:
    parts = pathlib.PurePosixPath(test_path).parts
    if len(parts) >= 2 and parts[0] == "tests":
        return parts[1]
    return "unknown"


def parse_tap_file(tap_path: pathlib.Path, raw_dir: pathlib.Path) -> tuple[list[Observation], list[Observation]]:
    relative = tap_path.relative_to(raw_dir).as_posix()
    if not relative.endswith(".tap"):
        raise ValueError(f"unexpected TAP path: {relative}")
    test_path = relative[: -len(".tap")]
    category = _category_for_test(test_path)

    observations: list[Observation] = []
    infrastructure: list[Observation] = []
    plan: int | None = None
    plan_skip = False
    next_number = 1

    for raw_line in tap_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        plan_match = PLAN_RE.match(line)
        if plan_match:
            plan = int(plan_match.group(1))
            plan_skip = plan == 0 and bool(plan_match.group(2))
            continue

        assertion_match = ASSERTION_RE.match(line)
        if not assertion_match:
            continue

        failed = bool(assertion_match.group(1))
        number = int(assertion_match.group(2)) if assertion_match.group(2) else next_number
        next_number = number + 1
        message = (assertion_match.group(3) or "").strip()
        directive = (assertion_match.group(4) or "").upper()
        directive_reason = (assertion_match.group(5) or "").strip()
        observations.append(
            Observation(
                assertion_id=f"{test_path}#{number}",
                test_path=test_path,
                category=category,
                number=number,
                result="fail" if failed else "pass",
                message=message,
                directive=directive,
                directive_reason=directive_reason,
            )
        )

    exit_path = tap_path.with_suffix(".exit")
    if exit_path.exists():
        try:
            exit_status = int(exit_path.read_text(encoding="utf-8").strip())
        except ValueError:
            exit_status = -1
        if exit_status != 0:
            infrastructure.append(
                Observation(
                    assertion_id=f"{test_path}#exit",
                    test_path=test_path,
                    category=category,
                    number=None,
                    result="fail",
                    message=f"test script exited with status {exit_status}",
                    classification="INFRASTRUCTURE",
                )
            )

    if plan_skip:
        return observations, infrastructure

    if plan is None:
        infrastructure.append(
            Observation(
                assertion_id=f"{test_path}#tap",
                test_path=test_path,
                category=category,
                number=None,
                result="fail",
                message="TAP plan missing",
                classification="INFRASTRUCTURE",
            )
        )
    else:
        observed_numbers = {observation.number for observation in observations}
        missing = [number for number in range(1, plan + 1) if number not in observed_numbers]
        if missing:
            preview = ",".join(str(number) for number in missing[:10])
            if len(missing) > 10:
                preview += ",..."
            infrastructure.append(
                Observation(
                    assertion_id=f"{test_path}#tap",
                    test_path=test_path,
                    category=category,
                    number=None,
                    result="fail",
                    message=f"incomplete TAP: planned {plan}, missing assertions {preview}",
                    classification="INFRASTRUCTURE",
                )
            )

    return observations, infrastructure


def collect_observations(raw_dir: pathlib.Path) -> tuple[list[Observation], list[Observation]]:
    tap_files = sorted(raw_dir.glob("tests/**/*.t.tap"))
    if not tap_files:
        return [], [
            Observation(
                assertion_id="harness#tap",
                test_path="harness",
                category="infrastructure",
                number=None,
                result="fail",
                message=f"no TAP files found below {raw_dir}",
                classification="INFRASTRUCTURE",
            )
        ]

    observations: list[Observation] = []
    infrastructure: list[Observation] = []
    for tap_path in tap_files:
        parsed, infra = parse_tap_file(tap_path, raw_dir)
        observations.extend(parsed)
        infrastructure.extend(infra)
    return observations, infrastructure


def classify(
    observations: list[Observation],
    infrastructure: list[Observation],
    supported: set[str],
    gaps: dict[str, Gap],
) -> list[Observation]:
    overlap = supported & gaps.keys()
    if overlap:
        joined = ", ".join(sorted(overlap)[:10])
        raise BaselineError(f"assertions cannot be both supported and known gaps: {joined}")

    seen: set[str] = set()
    for observation in observations:
        if observation.assertion_id in seen:
            infrastructure.append(
                Observation(
                    assertion_id=f"{observation.assertion_id}#duplicate",
                    test_path=observation.test_path,
                    category=observation.category,
                    number=observation.number,
                    result="fail",
                    message="duplicate TAP assertion ID",
                    classification="INFRASTRUCTURE",
                )
            )
            continue
        seen.add(observation.assertion_id)

        if observation.directive in {"TODO", "SKIP"}:
            if observation.assertion_id in supported or observation.assertion_id in gaps:
                observation.classification = "BASELINE_NOT_APPLICABLE"
                observation.message = (
                    f"baseline assertion became upstream {observation.directive}: "
                    f"{observation.directive_reason or observation.message}"
                )
            else:
                observation.classification = "UPSTREAM_NOT_APPLICABLE"
            continue

        gap = gaps.get(observation.assertion_id)
        if observation.result == "pass":
            if observation.assertion_id in supported:
                observation.classification = "PASS"
            elif gap:
                observation.classification = "XPASS"
                observation.issue = gap.issue
                observation.reason = gap.reason
            else:
                observation.classification = "UNCLASSIFIED_PASS"
        else:
            if observation.assertion_id in supported:
                observation.classification = "REGRESSION"
            elif gap:
                observation.classification = GAP_CATEGORIES[gap.category]
                observation.issue = gap.issue
                observation.reason = gap.reason
            else:
                observation.classification = "UNEXPECTED_FAIL"

    baseline_ids = supported | gaps.keys()
    for assertion_id in sorted(baseline_ids - seen):
        test_path = assertion_id.split("#", 1)[0]
        infrastructure.append(
            Observation(
                assertion_id=assertion_id,
                test_path=test_path,
                category=_category_for_test(test_path),
                number=None,
                result="missing",
                message="baseline assertion was not observed in this run",
                classification="BASELINE_NOT_OBSERVED",
            )
        )

    return observations + infrastructure


def _ratio(numerator: int, denominator: int) -> float | None:
    if denominator == 0:
        return None
    return numerator * 100.0 / denominator


def summarize(results: list[Observation], supported: set[str]) -> dict[str, object]:
    counts = collections.Counter(result.classification for result in results)
    relevant = [
        result
        for result in results
        if result.classification
        not in {"UPSTREAM_NOT_APPLICABLE", "INFRASTRUCTURE", "BASELINE_NOT_OBSERVED", "BASELINE_NOT_APPLICABLE"}
    ]
    classified = [
        result for result in relevant if result.classification not in {"UNCLASSIFIED_PASS", "UNEXPECTED_FAIL"}
    ]

    semantic_denominator = sum(
        counts[name]
        for name in (
            "PASS",
            "KNOWN_UNSUPPORTED",
            "KNOWN_SEMANTIC_DEFECT",
            "REGRESSION",
            "UNEXPECTED_FAIL",
            "UNCLASSIFIED_PASS",
        )
    )
    semantic_pass = counts["PASS"] + counts["UNCLASSIFIED_PASS"]
    supported_pass = sum(1 for result in results if result.classification == "PASS")
    observed_supported = sum(
        1 for result in results if result.classification in {"PASS", "REGRESSION"}
    )

    by_category: dict[str, dict[str, object]] = {}
    categories = sorted({result.category for result in results if result.category not in {"infrastructure", "unknown"}})
    for category in categories:
        category_results = [result for result in results if result.category == category]
        category_counts = collections.Counter(result.classification for result in category_results)
        denominator = sum(
            category_counts[name]
            for name in (
                "PASS",
                "KNOWN_UNSUPPORTED",
                "KNOWN_SEMANTIC_DEFECT",
                "REGRESSION",
                "UNEXPECTED_FAIL",
                "UNCLASSIFIED_PASS",
            )
        )
        by_category[category] = {
            "pass": category_counts["PASS"] + category_counts["UNCLASSIFIED_PASS"],
            "applicable": denominator,
            "support_percent": _ratio(
                category_counts["PASS"] + category_counts["UNCLASSIFIED_PASS"], denominator
            ),
        }

    blocking = [result for result in results if result.classification in BLOCKING_CLASSIFICATIONS]
    return {
        "counts": dict(sorted(counts.items())),
        "overall_support_percent": _ratio(semantic_pass, semantic_denominator),
        "supported_regression_pass_percent": _ratio(supported_pass, len(supported)),
        "observed_supported_pass_percent": _ratio(supported_pass, observed_supported),
        "classified_rate_percent": _ratio(len(classified), len(relevant)),
        "supported_set_size": len(supported),
        "relevant_assertions": len(relevant),
        "blocking_count": len(blocking),
        "by_category": by_category,
    }


def _format_percent(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.2f}%"


def render_markdown(payload: dict[str, object], results: list[Observation]) -> str:
    summary = payload["summary"]
    assert isinstance(summary, dict)
    counts = summary["counts"]
    assert isinstance(counts, dict)
    lines = [
        "# SwordFS POSIX conformance status",
        "",
        f"Latest run: **{'PASS' if summary['blocking_count'] == 0 else 'FAILED'}**",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Overall conformance support | {_format_percent(summary['overall_support_percent'])} |",
        f"| Supported regression pass | {_format_percent(summary['supported_regression_pass_percent'])} |",
        f"| Classified rate | {_format_percent(summary['classified_rate_percent'])} |",
        f"| Supported assertion count | {summary['supported_set_size']} |",
        f"| Blocking result count | {summary['blocking_count']} |",
        "",
        "## Classification counts",
        "",
        "| Classification | Count |",
        "| --- | ---: |",
    ]
    for name, count in sorted(counts.items()):
        lines.append(f"| {name} | {count} |")

    lines.extend(["", "## Per-category support", "", "| Category | PASS | Applicable | Support |", "| --- | ---: | ---: | ---: |"])
    by_category = summary["by_category"]
    assert isinstance(by_category, dict)
    for category, values in sorted(by_category.items()):
        assert isinstance(values, dict)
        lines.append(
            f"| {category} | {values['pass']} | {values['applicable']} | {_format_percent(values['support_percent'])} |"
        )

    blockers = [result for result in results if result.classification in BLOCKING_CLASSIFICATIONS]
    if blockers:
        lines.extend(["", "## Blocking results", "", "| Assertion | Classification | Detail |", "| --- | --- | --- |"])
        for result in blockers[:200]:
            detail = result.message or result.reason or "-"
            detail = detail.replace("|", "\\|").replace("\n", " ")
            lines.append(f"| `{result.assertion_id}` | {result.classification} | {detail} |")
        if len(blockers) > 200:
            lines.append(f"\n_Only the first 200 of {len(blockers)} blocking results are shown; see `result.json`._")

    known_gaps = [
        result
        for result in results
        if result.classification in {"KNOWN_UNSUPPORTED", "KNOWN_SEMANTIC_DEFECT", "ENVIRONMENT"}
    ]
    if known_gaps:
        gap_groups = collections.Counter(
            (result.issue, result.classification, result.reason) for result in known_gaps
        )
        lines.extend(
            [
                "",
                "## Known-gap Issues",
                "",
                "| Issue | Classification | Assertions | Reason |",
                "| --- | --- | ---: | --- |",
            ]
        )
        for (issue, classification, reason), count in sorted(gap_groups.items()):
            issue_link = issue or "-"
            if re.fullmatch(r"#\d+", issue_link):
                issue_number = issue_link[1:]
                issue_link = f"[{issue_link}](https://github.com/SwordInfra/SwordFS/issues/{issue_number})"
            safe_reason = (reason or "-").replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {issue_link} | {classification} | {count} | {safe_reason} |")

        lines.extend(
            [
                "",
                "## Known-gap assertions",
                "",
                "| Assertion | Classification | Issue | Reason |",
                "| --- | --- | --- | --- |",
            ]
        )
        for result in known_gaps[:200]:
            issue = result.issue or "-"
            if re.fullmatch(r"#\d+", issue):
                issue_number = issue[1:]
                issue = f"[{issue}](https://github.com/SwordInfra/SwordFS/issues/{issue_number})"
            reason = (result.reason or result.message or "-").replace("|", "\\|").replace("\n", " ")
            lines.append(f"| `{result.assertion_id}` | {result.classification} | {issue} | {reason} |")
        if len(known_gaps) > 200:
            lines.append(f"\n_Only the first 200 of {len(known_gaps)} known gaps are shown; see `result.json`._")

    metadata = payload["metadata"]
    assert isinstance(metadata, dict)
    lines.extend(["", "## Baseline metadata", ""])
    lines.append(f"- SwordFS commit: `{metadata.get('swordfs_commit', 'unknown')}`")
    lines.append(f"- pjdfstest commit: `{metadata.get('pjdfstest_commit', 'unknown')}`")
    environment = metadata.get("environment", {})
    if isinstance(environment, dict):
        for key in sorted(environment):
            lines.append(f"- {key}: `{environment[key]}`")
    lines.append("")
    return "\n".join(lines)


def write_bootstrap(output_dir: pathlib.Path, results: list[Observation]) -> None:
    supported_candidates = sorted(
        result.assertion_id for result in results if result.classification == "UNCLASSIFIED_PASS"
    )
    (output_dir / "bootstrap-supported.txt").write_text(
        "".join(f"{selector}\n" for selector in compact_assertion_ids(supported_candidates)), encoding="utf-8"
    )

    gap_lines = ["selector\tcategory\tissue\treason\n"]
    unexpected = [result for result in results if result.classification == "UNEXPECTED_FAIL"]
    by_path: dict[str, list[Observation]] = collections.defaultdict(list)
    for result in unexpected:
        by_path[result.test_path].append(result)
    for test_path in sorted(by_path):
        selector = compact_assertion_ids(result.assertion_id for result in by_path[test_path])[0]
        gap_lines.append(f"{selector}\tunknown\t\tclassify initial pjdfstest failure group\n")
    (output_dir / "bootstrap-gaps.tsv").write_text("".join(gap_lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw-dir", type=pathlib.Path, required=True)
    parser.add_argument("--supported-file", type=pathlib.Path, required=True)
    parser.add_argument("--previous-supported-file", type=pathlib.Path)
    parser.add_argument("--allow-supported-removal", action="store_true")
    parser.add_argument("--known-gaps-file", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--swordfs-commit", required=True)
    parser.add_argument("--pjdfstest-commit", required=True)
    parser.add_argument("--environment-json", type=pathlib.Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    try:
        supported = load_supported(args.supported_file)
        if args.previous_supported_file and args.previous_supported_file.exists():
            previous_supported = load_supported(args.previous_supported_file)
            validate_supported_monotonicity(supported, previous_supported, args.allow_supported_removal)
        gaps = load_gaps(args.known_gaps_file)
        observations, infrastructure = collect_observations(args.raw_dir)
        results = classify(observations, infrastructure, supported, gaps)
    except BaselineError as error:
        print(f"baseline error: {error}", file=sys.stderr)
        return 2

    environment: dict[str, object] = {}
    if args.environment_json and args.environment_json.exists():
        environment = json.loads(args.environment_json.read_text(encoding="utf-8"))

    summary = summarize(results, supported)
    payload = {
        "metadata": {
            "swordfs_commit": args.swordfs_commit,
            "pjdfstest_commit": args.pjdfstest_commit,
            "environment": environment,
        },
        "summary": summary,
        "tests": [result.as_dict() for result in results],
    }
    (args.output_dir / "result.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.output_dir / "report.md").write_text(render_markdown(payload, results), encoding="utf-8")
    write_bootstrap(args.output_dir, results)

    print(render_markdown(payload, results))
    if args.strict and summary["blocking_count"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
