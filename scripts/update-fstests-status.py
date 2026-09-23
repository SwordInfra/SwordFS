#!/usr/bin/env python3
"""Build the stable main-branch fstests status page and history."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib


RAW_RESULT_NAMES = ("PASS", "FAIL", "NOTRUN", "DEFERRED")


def load_history(path: pathlib.Path) -> list[dict[str, object]]:
    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("history must be a JSON array")
    return payload


def raw_result_count(item: dict[str, object], name: str) -> int:
    raw_results = item.get("raw_results", {})
    if not isinstance(raw_results, dict):
        return 0
    return int(raw_results.get(name, 0))


def trend_history(history: list[dict[str, object]]) -> list[dict[str, object]]:
    trend: list[dict[str, object]] = []
    previous_metrics: tuple[object, ...] | None = None
    for item in history:
        if item.get("selected_count") is None:
            continue
        metrics = (
            item.get("status"),
            item.get("selected_count"),
            item.get("executed_count"),
            item.get("supported_count"),
            item.get("overall_support_percent"),
            item.get("known_gap_count"),
            item.get("deferred_count"),
            *(raw_result_count(item, name) for name in RAW_RESULT_NAMES),
        )
        if metrics != previous_metrics:
            trend.append(item)
            previous_metrics = metrics
    return trend


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--history", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--swordfs-commit", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--infra-failure", default="")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    history = load_history(args.history)
    newest_run_id = max((int(item.get("run_id", 0)) for item in history), default=0)
    if newest_run_id > args.run_id:
        print(
            f"Skipping stale fstests status publication for run {args.run_id}; "
            f"newer run {newest_run_id} is already published"
        )
        return 0
    timestamp = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()

    result: dict[str, object] | None = None
    if args.result and args.result.exists():
        result = json.loads(args.result.read_text(encoding="utf-8"))

    if result:
        metadata = result.get("metadata", {})
        summary = result.get("summary", {})
        assert isinstance(metadata, dict) and isinstance(summary, dict)
        raw_results = summary.get("raw_results", {})
        classifications = summary.get("classifications", {})
        assert isinstance(raw_results, dict) and isinstance(classifications, dict)
        status = "PASS" if int(summary.get("blocking_count", 0)) == 0 else "BLOCKED"
        entry = {
            "timestamp": timestamp,
            "swordfs_commit": metadata.get("swordfs_commit", args.swordfs_commit),
            "fstests_commit": metadata.get("fstests_commit", "unknown"),
            "fstests_group": metadata.get("fstests_group", "unknown"),
            "status": status,
            "selected_count": summary.get("selected_count"),
            "executed_count": summary.get("executed_count"),
            "deferred_count": summary.get("deferred_count"),
            "supported_count": summary.get("supported_count"),
            "known_gap_count": summary.get("known_gap_count"),
            "supported_gate_percent": summary.get("supported_gate_percent"),
            "overall_support_percent": summary.get("overall_support_percent"),
            "executed_classified_percent": summary.get("executed_classified_percent"),
            "blocking_count": summary.get("blocking_count"),
            "raw_results": raw_results,
            "classifications": classifications,
            "run_url": args.run_url,
            "run_id": args.run_id,
        }
        latest = result
        if args.report and args.report.exists():
            current_markdown = args.report.read_text(encoding="utf-8").rstrip()
        else:
            current_markdown = (
                f"# SwordFS fstests conformance\n\nBaseline gate: **{status}**\n\n"
                "A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; "
                "it does not mean every selected testcase passes."
            )
    else:
        status = "INFRASTRUCTURE_FAILED"
        entry = {
            "timestamp": timestamp,
            "swordfs_commit": args.swordfs_commit,
            "fstests_commit": "unknown",
            "fstests_group": "unknown",
            "status": status,
            "selected_count": None,
            "executed_count": None,
            "deferred_count": None,
            "supported_count": None,
            "known_gap_count": None,
            "supported_gate_percent": None,
            "overall_support_percent": None,
            "executed_classified_percent": None,
            "blocking_count": None,
            "raw_results": {},
            "classifications": {},
            "run_url": args.run_url,
            "run_id": args.run_id,
        }
        latest = {
            "metadata": {"swordfs_commit": args.swordfs_commit},
            "status": status,
            "reason": args.infra_failure,
        }
        current_markdown = "\n".join(
            [
                "# SwordFS fstests conformance",
                "",
                "Latest main fstests run: **INFRASTRUCTURE FAILED**",
                "",
                args.infra_failure or "The fstests job ended before a classified report was produced.",
                "",
                f"CI run: {args.run_url}",
            ]
        )

    history = [
        item
        for item in history
        if not (
            item.get("swordfs_commit") == entry["swordfs_commit"]
            and item.get("run_url") == entry["run_url"]
        )
    ]
    history.append(entry)
    history = history[-200:]

    healthy = next((item for item in reversed(history) if item.get("status") == "PASS"), None)
    trend_lines = [
        "",
        f"Authoritative CI run: {args.run_url}",
        f"Recorded at: {timestamp}",
        "",
        "## Historical main-branch progress",
        "",
        "| Time | SwordFS | Status | Overall support | Supported | PASS | FAIL | NOTRUN | Deferred | Known gaps |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for item in trend_history(history)[-30:]:
        commit = str(item.get("swordfs_commit", "unknown"))
        trend_lines.append(
            "| {timestamp} | `{commit}` | {status} | {overall} | {supported} | {passed} | {failed} | {notrun} | {deferred} | {gaps} |".format(
                timestamp=item.get("timestamp", "unknown"),
                commit=commit[:12],
                status=item.get("status", "unknown"),
                overall=(
                    "n/a"
                    if item.get("overall_support_percent") is None
                    else f"{float(item['overall_support_percent']):.2f}%"
                ),
                supported=item.get("supported_count", "n/a"),
                passed=raw_result_count(item, "PASS"),
                failed=raw_result_count(item, "FAIL"),
                notrun=raw_result_count(item, "NOTRUN"),
                deferred=raw_result_count(item, "DEFERRED"),
                gaps=item.get("known_gap_count", "n/a"),
            )
        )
    if healthy:
        trend_lines.extend(
            [
                "",
                f"Last fully healthy baseline: `{str(healthy.get('swordfs_commit', 'unknown'))}`",
            ]
        )

    (args.output_dir / "status.md").write_text(current_markdown + "\n" + "\n".join(trend_lines) + "\n", encoding="utf-8")
    (args.output_dir / "history.json").write_text(json.dumps(history, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.output_dir / "latest.json").write_text(json.dumps(latest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
